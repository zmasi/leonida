"""tools/spirv_metal_set0.py: set 0's descriptor arrays get fixed, non-overlapping Metal IDs (SHADER-161)."""
import os
import shutil
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import spirv_metal_set0 as m  # noqa: E402

# The set-0 part of a translated pixel shader (spirv-dis, names kept): sampled 2D images, samplers, the hash table.
MODULE = """\
               OpDecorate %images DescriptorSet 0
               OpDecorate %images Binding 0
               OpDecorate %samplers DescriptorSet 0
               OpDecorate %samplers Binding 1
               OpDecorate %desc_map DescriptorSet 0
               OpDecorate %desc_map Binding 2
       %uint = OpTypeInt 32 0
      %float = OpTypeFloat 32
      %img2d = OpTypeImage %float 2D 0 0 0 1 Unknown
        %smp = OpTypeSampler
     %rt_img = OpTypeRuntimeArray %img2d
     %rt_smp = OpTypeRuntimeArray %smp
    %ptr_img = OpTypePointer UniformConstant %rt_img
    %ptr_smp = OpTypePointer UniformConstant %rt_smp
     %rt_map = OpTypeRuntimeArray %uint
        %map = OpTypeStruct %rt_map
    %ptr_map = OpTypePointer StorageBuffer %map
     %images = OpVariable %ptr_img UniformConstant
   %samplers = OpVariable %ptr_smp UniformConstant
   %desc_map = OpVariable %ptr_map StorageBuffer
"""


def test_capacities_match_vulkan_bindless():
    counts = m.capacities()
    assert counts[:5] == [4096, 1024, 1, 1024, 1024]
    assert counts[26:] == [256, 256, 256]  # storage-only Texture1DArray UAVs


def test_metal_ids_are_running_sums():
    assert m.metal_ids([4096, 1024, 1, 1024]) == [0, 4096, 5120, 5121]


def test_rewrite_sizes_arrays_and_spaces_bindings():
    counts = m.capacities()
    out = m.rewrite(MODULE, counts)
    assert "OpDecorate %images Binding 0" in out
    assert "OpDecorate %samplers Binding 4096" in out
    assert "OpDecorate %desc_map Binding 5120" in out
    assert "%msl_cap_b0 = OpConstant %uint 4096" in out
    assert "%msl_arr_b0 = OpTypeArray %img2d %msl_cap_b0" in out
    assert "%images = OpVariable %msl_ptr_b0 UniformConstant" in out
    assert "%msl_arr_b1 = OpTypeArray %smp %msl_cap_b1" in out
    assert "%samplers = OpVariable %msl_ptr_b1 UniformConstant" in out
    # the hash table is a storage buffer, not a descriptor array: only its binding moves
    assert "%desc_map = OpVariable %ptr_map StorageBuffer" in out
    assert "msl_arr_b2" not in out
    # each new type comes after the pointer type it replaces, so its operands are already declared
    assert out.index("%ptr_img = OpTypePointer") < out.index("%msl_cap_b0")


def test_help_is_a_clean_exit_zero(capsys):
    for flag in ("-h", "--help"):
        assert m.main(["spirv_metal_set0.py", flag]) == 0
        assert "Usage: spirv_metal_set0.py" in capsys.readouterr().out


# Both sampler arrays use the same input type, but only the set-0 array is resized.
SAMPLE_FRAGMENT = """\
OpCapability Shader
OpCapability RuntimeDescriptorArray
OpExtension "SPV_EXT_descriptor_indexing"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %images %samplers %foreign_samplers %color
OpExecutionMode %main OriginUpperLeft
OpName %images "images"
OpName %samplers "samplers"
OpName %foreign_samplers "foreign_samplers"
OpName %foreign_copy "foreign_copy"
OpDecorate %images DescriptorSet 0
OpDecorate %images Binding 0
OpDecorate %samplers DescriptorSet 0
OpDecorate %samplers Binding 1
OpDecorate %foreign_samplers DescriptorSet 1
OpDecorate %foreign_samplers Binding 1
OpDecorate %color Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v2f = OpTypeVector %float 2
%v4f = OpTypeVector %float 4
%image = OpTypeImage %float 2D 0 0 0 1 Unknown
%smp = OpTypeSampler
%sampled = OpTypeSampledImage %image
%image_array = OpTypeRuntimeArray %image
%sampler_array = OpTypeRuntimeArray %smp
%images_ptr = OpTypePointer UniformConstant %image_array
%samplers_ptr = OpTypePointer UniformConstant %sampler_array
%image_ptr = OpTypePointer UniformConstant %image
%sampler_ptr = OpTypePointer UniformConstant %smp
%color_ptr = OpTypePointer Output %v4f
%zero = OpConstant %uint 0
%half = OpConstant %float 0.5
%uv = OpConstantComposite %v2f %half %half
%images = OpVariable %images_ptr UniformConstant
%samplers = OpVariable %samplers_ptr UniformConstant
%foreign_samplers = OpVariable %samplers_ptr UniformConstant
%color = OpVariable %color_ptr Output
%main = OpFunction %void None %fn
%entry = OpLabel
%foreign_copy = OpCopyObject %samplers_ptr %foreign_samplers
%ip = OpAccessChain %image_ptr %images %zero
%sp = OpAccessChain %sampler_ptr %samplers %zero
%fp = OpAccessChain %sampler_ptr %foreign_copy %zero
%i = OpLoad %image %ip
%s = OpLoad %smp %sp
%f = OpLoad %smp %fp
%local_texture = OpSampledImage %sampled %i %s
%foreign_texture = OpSampledImage %sampled %i %f
%a = OpImageSampleImplicitLod %v4f %local_texture %uv
%b = OpImageSampleImplicitLod %v4f %foreign_texture %uv
%rgba = OpFAdd %v4f %a %b
OpStore %color %rgba
OpReturn
OpFunctionEnd
"""


@pytest.mark.skipif(not all(shutil.which(tool) for tool in ("spirv-as", "spirv-dis", "spirv-val")),
                    reason="needs SPIRV-Tools on PATH")
@pytest.mark.parametrize("resource,binding,depth", [
    ("samplers", 1, 0),
    ("samplers", 1, 1),
    ("samplers", 1, 2),
    ("images", 3, 1),
], ids=["direct", "sampler-copy", "sampler-chain", "image-copy"])
def test_cli_retypes_descriptor_array_copies(tmp_path, resource, binding, depth):
    assembly = SAMPLE_FRAGMENT
    if resource == "images":
        assembly = assembly.replace("OpDecorate %images Binding 0", f"OpDecorate %images Binding {binding}")
    base = f"%{resource}"
    for i in range(depth):
        copy = f"%copy{i}"
        assembly = assembly.replace(
            "OpDecorate %images DescriptorSet 0",
            f'OpName {copy} "copy{i}"\nOpDecorate %images DescriptorSet 0',
        ).replace(
            f" {base} %zero",
            f" {copy} %zero",
        ).replace(
            "%foreign_copy = OpCopyObject",
            f"{copy} = OpCopyObject %{resource}_ptr {base}\n%foreign_copy = OpCopyObject",
        )
        base = copy

    src, dst = tmp_path / "input.spv", tmp_path / "metal.spv"
    target = ["--target-env", "vulkan1.2"]
    subprocess.run(["spirv-as", *target, "-", "-o", str(src)], input=assembly, text=True, check=True)
    subprocess.run(["spirv-val", *target, str(src)], check=True)
    subprocess.run([sys.executable, "-I", m.__file__, str(src), str(dst)], check=True)
    subprocess.run(["spirv-val", *target, str(dst)], check=True)

    text = subprocess.check_output(["spirv-dis", str(dst)], text=True)
    base = f"%{resource}"
    definitions = {parts[0]: parts[2:] for parts in map(str.split, text.splitlines())
                   if len(parts) > 2 and parts[1] == "="}
    local_type = definitions[base][1]
    assert definitions[definitions[local_type][2]][0] == "OpTypeArray"
    assert f"OpDecorate {base} Binding {m.metal_ids(m.capacities())[binding]}" in text
    for i in range(depth):
        copy = f"%copy{i}"
        assert definitions[copy] == ["OpCopyObject", local_type, base]
        base = copy
    foreign_type = definitions["%foreign_samplers"][1]
    assert definitions[definitions[foreign_type][2]][0] == "OpTypeRuntimeArray"
    assert definitions["%foreign_copy"] == ["OpCopyObject", foreign_type, "%foreign_samplers"]


def _assemble_rewrite_and_validate(tmp_path, assembly):
    src, dst = tmp_path / "input.spv", tmp_path / "metal.spv"
    target = ["--target-env", "vulkan1.2"]
    subprocess.run(["spirv-as", *target, "-", "-o", str(src)], input=assembly, text=True,
                   encoding="utf-8", check=True)
    subprocess.run(["spirv-val", *target, str(src)], check=True)
    assert m.main(["spirv_metal_set0.py", str(src), str(dst)]) == 0
    subprocess.run(["spirv-val", *target, str(dst)], check=True)
    return subprocess.check_output(["spirv-dis", str(dst)], text=True, encoding="utf-8")


@pytest.mark.skipif(not all(shutil.which(tool) for tool in ("spirv-as", "spirv-dis", "spirv-val")),
                    reason="needs SPIRV-Tools on PATH")
def test_synthesizes_uint_type_when_module_only_uses_signed_int(tmp_path):
    signed_module = (
        SAMPLE_FRAGMENT
        .replace("%uint = OpTypeInt 32 0", "%int = OpTypeInt 32 1")
        .replace("%zero = OpConstant %uint 0", "%zero = OpConstant %int 0")
    )
    text = _assemble_rewrite_and_validate(tmp_path, signed_module)
    definitions = {parts[0]: parts[2:] for parts in map(str.split, text.splitlines())
                   if len(parts) > 2 and parts[1] == "="}
    arr_id = definitions[definitions["%images"][1]][2]
    assert definitions[arr_id][0] == "OpTypeArray"
    cap_id = definitions[arr_id][2]
    assert definitions[definitions[cap_id][1]] == ["OpTypeInt", "32", "0"]
    assert text.index("= OpTypeInt 32 0") < text.index(f"{cap_id} = OpConstant")


@pytest.mark.skipif(not all(shutil.which(tool) for tool in ("spirv-as", "spirv-dis", "spirv-val")),
                    reason="needs SPIRV-Tools on PATH")
def test_retypes_helper_function_parameters_and_nested_calls(tmp_path):
    assembly = """\
OpCapability Shader
OpCapability RuntimeDescriptorArray
OpExtension "SPV_EXT_descriptor_indexing"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %images %samplers %foreign_samplers %color
OpExecutionMode %main OriginUpperLeft
OpName %images "images"
OpName %samplers "samplers"
OpName %foreign_samplers "foreign_samplers"
OpName %loc_smp "loc_smp"
OpName %loc_smp_copy "loc_smp_copy"
OpName %in_sp_arr "in_sp_arr"
OpName %for_smp "for_smp"
OpDecorate %images DescriptorSet 0
OpDecorate %images Binding 0
OpDecorate %samplers DescriptorSet 0
OpDecorate %samplers Binding 1
OpDecorate %foreign_samplers DescriptorSet 1
OpDecorate %foreign_samplers Binding 1
OpDecorate %color Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v2f = OpTypeVector %float 2
%v4f = OpTypeVector %float 4
%image = OpTypeImage %float 2D 0 0 0 1 Unknown
%smp = OpTypeSampler
%sampled = OpTypeSampledImage %image
%image_array = OpTypeRuntimeArray %image
%sampler_array = OpTypeRuntimeArray %smp
%images_ptr = OpTypePointer UniformConstant %image_array
%samplers_ptr = OpTypePointer UniformConstant %sampler_array
%image_ptr = OpTypePointer UniformConstant %image
%sampler_ptr = OpTypePointer UniformConstant %smp
%color_ptr = OpTypePointer Output %v4f
%smp_helper_ty = OpTypeFunction %v4f %image_ptr %samplers_ptr
%helper_ty = OpTypeFunction %v4f %samplers_ptr
%zero = OpConstant %uint 0
%half = OpConstant %float 0.5
%uv = OpConstantComposite %v2f %half %half
%images = OpVariable %images_ptr UniformConstant
%samplers = OpVariable %samplers_ptr UniformConstant
%foreign_samplers = OpVariable %samplers_ptr UniformConstant
%color = OpVariable %color_ptr Output
%inner_sample = OpFunction %v4f None %smp_helper_ty
%in_ip = OpFunctionParameter %image_ptr
%in_sp_arr = OpFunctionParameter %samplers_ptr
%inner_bb = OpLabel
%i0 = OpLoad %image %in_ip
%sp0 = OpAccessChain %sampler_ptr %in_sp_arr %zero
%s0 = OpLoad %smp %sp0
%tex0 = OpSampledImage %sampled %i0 %s0
%r0 = OpImageSampleImplicitLod %v4f %tex0 %uv
OpReturnValue %r0
OpFunctionEnd
%helper_local = OpFunction %v4f None %helper_ty
%loc_smp = OpFunctionParameter %samplers_ptr
%hl_bb = OpLabel
%loc_smp_copy = OpCopyObject %samplers_ptr %loc_smp
%loc_ip = OpAccessChain %image_ptr %images %zero
%r1 = OpFunctionCall %v4f %inner_sample %loc_ip %loc_smp_copy
OpReturnValue %r1
OpFunctionEnd
%helper_foreign = OpFunction %v4f None %helper_ty
%for_smp = OpFunctionParameter %samplers_ptr
%hf_bb = OpLabel
%for_ip = OpAccessChain %image_ptr %images %zero
%for_sp = OpAccessChain %sampler_ptr %for_smp %zero
%i1 = OpLoad %image %for_ip
%s1 = OpLoad %smp %for_sp
%tex1 = OpSampledImage %sampled %i1 %s1
%r2 = OpImageSampleImplicitLod %v4f %tex1 %uv
OpReturnValue %r2
OpFunctionEnd
%main = OpFunction %void None %fn
%entry = OpLabel
%a = OpFunctionCall %v4f %helper_local %samplers
%b = OpFunctionCall %v4f %helper_foreign %foreign_samplers
%rgba = OpFAdd %v4f %a %b
OpStore %color %rgba
OpReturn
OpFunctionEnd
"""
    text = _assemble_rewrite_and_validate(tmp_path, assembly)
    definitions = {parts[0]: parts[2:] for parts in map(str.split, text.splitlines())
                   if len(parts) > 2 and parts[1] == "="}
    resized_smp_ptr = definitions["%samplers"][1]
    assert definitions[definitions[resized_smp_ptr][2]][0] == "OpTypeArray"
    assert definitions["%loc_smp"] == ["OpFunctionParameter", resized_smp_ptr]
    assert definitions["%loc_smp_copy"] == ["OpCopyObject", resized_smp_ptr, "%loc_smp"]
    assert definitions["%in_sp_arr"] == ["OpFunctionParameter", resized_smp_ptr]
    foreign_smp_ptr = definitions["%foreign_samplers"][1]
    assert definitions[definitions[foreign_smp_ptr][2]][0] == "OpTypeRuntimeArray"
    assert definitions["%for_smp"] == ["OpFunctionParameter", foreign_smp_ptr]


def test_rejects_helper_called_with_conflicting_pointer_types():
    dis = """\
               OpDecorate %samplers DescriptorSet 0
               OpDecorate %samplers Binding 1
               OpDecorate %foreign_samplers DescriptorSet 1
               OpDecorate %foreign_samplers Binding 1
       %void = OpTypeVoid
       %uint = OpTypeInt 32 0
      %float = OpTypeFloat 32
        %smp = OpTypeSampler
     %rt_smp = OpTypeRuntimeArray %smp
    %ptr_smp = OpTypePointer UniformConstant %rt_smp
      %fn_ty = OpTypeFunction %void %ptr_smp
   %samplers = OpVariable %ptr_smp UniformConstant
%foreign_samplers = OpVariable %ptr_smp UniformConstant
     %helper = OpFunction %void None %fn_ty
      %p_smp = OpFunctionParameter %ptr_smp
         %bb = OpLabel
               OpReturn
               OpFunctionEnd
       %main = OpFunction %void None %fn_ty
      %entry = OpLabel
         %c1 = OpFunctionCall %void %helper %samplers
         %c2 = OpFunctionCall %void %helper %foreign_samplers
               OpReturn
               OpFunctionEnd
"""
    with pytest.raises(SystemExit, match="conflicting descriptor array types"):
        m.rewrite(dis, m.capacities())


@pytest.mark.skipif(not all(shutil.which(tool) for tool in ("spirv-as", "spirv-dis", "spirv-val")),
                    reason="needs SPIRV-Tools on PATH")
def test_rewrites_set0_variables_decorated_via_decoration_group(tmp_path):
    assembly = """\
OpCapability Shader
OpCapability RuntimeDescriptorArray
OpExtension "SPV_EXT_descriptor_indexing"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %images %samplers %foreign_samplers %color
OpExecutionMode %main OriginUpperLeft
OpName %images "images"
OpName %samplers "samplers"
OpName %foreign_samplers "foreign_samplers"
OpDecorate %set0_grp DescriptorSet 0
OpDecorate %shared_b1_grp Binding 1
OpDecorate %images Binding 0
OpDecorate %foreign_samplers DescriptorSet 1
OpDecorate %color Location 0
%set0_grp = OpDecorationGroup
%shared_b1_grp = OpDecorationGroup
OpGroupDecorate %set0_grp %images %samplers
OpGroupDecorate %shared_b1_grp %samplers %foreign_samplers
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v2f = OpTypeVector %float 2
%v4f = OpTypeVector %float 4
%image = OpTypeImage %float 2D 0 0 0 1 Unknown
%smp = OpTypeSampler
%sampled = OpTypeSampledImage %image
%image_array = OpTypeRuntimeArray %image
%sampler_array = OpTypeRuntimeArray %smp
%images_ptr = OpTypePointer UniformConstant %image_array
%samplers_ptr = OpTypePointer UniformConstant %sampler_array
%image_ptr = OpTypePointer UniformConstant %image
%sampler_ptr = OpTypePointer UniformConstant %smp
%color_ptr = OpTypePointer Output %v4f
%zero = OpConstant %uint 0
%half = OpConstant %float 0.5
%uv = OpConstantComposite %v2f %half %half
%images = OpVariable %images_ptr UniformConstant
%samplers = OpVariable %samplers_ptr UniformConstant
%foreign_samplers = OpVariable %samplers_ptr UniformConstant
%color = OpVariable %color_ptr Output
%main = OpFunction %void None %fn
%entry = OpLabel
%ip = OpAccessChain %image_ptr %images %zero
%sp = OpAccessChain %sampler_ptr %samplers %zero
%fp = OpAccessChain %sampler_ptr %foreign_samplers %zero
%i = OpLoad %image %ip
%s = OpLoad %smp %sp
%f = OpLoad %smp %fp
%t0 = OpSampledImage %sampled %i %s
%t1 = OpSampledImage %sampled %i %f
%a = OpImageSampleImplicitLod %v4f %t0 %uv
%b = OpImageSampleImplicitLod %v4f %t1 %uv
%rgba = OpFAdd %v4f %a %b
OpStore %color %rgba
OpReturn
OpFunctionEnd
"""
    text = _assemble_rewrite_and_validate(tmp_path, assembly)
    assert "OpDecorate %images Binding 0" in text
    assert "OpDecorate %samplers Binding 4096" in text
    assert "OpDecorate %foreign_samplers Binding 1" in text
    definitions = {parts[0]: parts[2:] for parts in map(str.split, text.splitlines())
                   if len(parts) > 2 and parts[1] == "="}
    assert definitions[definitions[definitions["%images"][1]][2]][0] == "OpTypeArray"
    assert definitions[definitions[definitions["%samplers"][1]][2]][0] == "OpTypeArray"
    assert definitions[definitions[definitions["%foreign_samplers"][1]][2]][0] == "OpTypeRuntimeArray"


def test_rejects_retyped_pointer_in_phi_or_select():
    base = """\
               OpDecorate %samplers DescriptorSet 0
               OpDecorate %samplers Binding 1
       %bool = OpTypeBool
       %void = OpTypeVoid
       %uint = OpTypeInt 32 0
        %smp = OpTypeSampler
     %rt_smp = OpTypeRuntimeArray %smp
    %ptr_smp = OpTypePointer UniformConstant %rt_smp
       %true = OpConstantTrue %bool
   %samplers = OpVariable %ptr_smp UniformConstant
"""
    phi_dis = base + "        %phi = OpPhi %ptr_smp %samplers %entry\n"
    sel_dis = base + "        %sel = OpSelect %ptr_smp %true %samplers %samplers\n"
    with pytest.raises(SystemExit, match="OpPhi"):
        m.rewrite(phi_dis, m.capacities())
    with pytest.raises(SystemExit, match="OpSelect"):
        m.rewrite(sel_dis, m.capacities())


def test_cli_captures_spirv_as_stderr(tmp_path, capsys, monkeypatch):
    src, dst = tmp_path / "in.spv", tmp_path / "out.spv"
    src.write_bytes(b"")

    def fake_run(cmd, **kwargs):
        if cmd[0] == "spirv-dis":
            return subprocess.CompletedProcess(cmd, 0, stdout=MODULE, stderr="")
        assert kwargs.get("capture_output") is True
        assert kwargs.get("encoding") == "utf-8"
        raise subprocess.CalledProcessError(1, cmd, stderr="error: line 9: mock spirv-as failure\n")

    monkeypatch.setattr(m, "_tools", lambda: ("spirv-dis", "spirv-as"))
    monkeypatch.setattr(m, "_run", fake_run)
    rc = m.cli(["spirv_metal_set0.py", str(src), str(dst)])
    assert rc == 1
    assert "mock spirv-as failure" in capsys.readouterr().err


@pytest.mark.skipif(not all(shutil.which(tool) for tool in ("spirv-as", "spirv-dis", "spirv-val")),
                    reason="needs SPIRV-Tools on PATH")
def test_cli_preserves_utf8_opnames(tmp_path):
    assembly = SAMPLE_FRAGMENT.replace('OpName %images "images"', 'OpName %images "Schlüssel_αβγ"')
    text = _assemble_rewrite_and_validate(tmp_path, assembly)
    assert "Schlüssel_αβγ" in text
