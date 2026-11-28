"""Run the SHADER-605 direct OpImageWrite probe with the supplied vk_store_test binary."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

TIMEOUT = 60


# Hand-written compute shader. Only the image format changes between runs.
MODULE = """OpCapability Shader
OpCapability StorageImageWriteWithoutFormat
OpCapability SignedZeroInfNanPreserve
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %image
OpExecutionMode %main LocalSize 1 1 1
OpExecutionMode %main SignedZeroInfNanPreserve 32
OpDecorate %image DescriptorSet 0
OpDecorate %image Binding 8
%void = OpTypeVoid
%fn = OpTypeFunction %void
%i = OpTypeInt 32 1
%f = OpTypeFloat 32
%xy = OpTypeVector %i 2
%rgba = OpTypeVector %f 4
%img = OpTypeImage %f 2D 0 0 0 2 {image_format}
%ptr = OpTypePointer UniformConstant %img
%image = OpVariable %ptr UniformConstant
%zero = OpConstant %i 0
%one = OpConstant %i 1
%two = OpConstant %i 2
%a = OpConstantComposite %xy %zero %zero
%b = OpConstantComposite %xy %one %zero
%c = OpConstantComposite %xy %two %zero
%n = OpConstant %f -0.0
%p = OpConstant %f 0.0
%f1 = OpConstant %f 1.0
%va = OpConstantComposite %rgba %n %p %p %p
%vb = OpConstantComposite %rgba %n %f1 %p %p
%vc = OpConstantComposite %rgba %n %n %p %p
%main = OpFunction %void None %fn
%entry = OpLabel
%handle = OpLoad %img %image
OpImageWrite %handle %a %va
OpImageWrite %handle %b %vb
OpImageWrite %handle %c %vc
OpReturn
OpFunctionEnd
"""


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("harness", help="path to vk_store_test executable")
    args = parser.parse_args(argv)
    if not args.harness.strip():
        parser.error("harness path must not be empty")
    harness_path = Path(args.harness)
    if not harness_path.is_file():
        print(f"ERROR: harness not found or not a file: {args.harness}", file=sys.stderr)
        return 2
    harness = harness_path.resolve()
    results = []
    with tempfile.TemporaryDirectory() as scratch:
        for image_format in ("Unknown", "Rgba32f"):
            print(image_format, flush=True)
            source = Path(scratch) / f"{image_format}.spvasm"
            binary = source.with_suffix(".spv")
            source.write_text(MODULE.format(image_format=image_format), encoding="utf-8", newline="\n")
            try:
                subprocess.run(
                    ["spirv-as", "--target-env", "vulkan1.2", str(source), "-o", str(binary)],
                    check=True,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=TIMEOUT,
                )
                subprocess.run(
                    ["spirv-val", "--target-env", "vulkan1.2", str(binary)],
                    check=True,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=TIMEOUT,
                )
                proc = subprocess.run(
                    [str(harness), str(binary), "signed-zero"],
                    timeout=TIMEOUT,
                )
            except subprocess.CalledProcessError as exc:
                detail = (exc.stderr or "").strip()
                msg = f"{exc}: {detail}" if detail else str(exc)
                print(f"ERROR: {image_format} probe failed: {msg}", file=sys.stderr)
                return 1
            except (OSError, subprocess.SubprocessError) as exc:
                print(f"ERROR: {image_format} probe failed: {exc}", file=sys.stderr)
                return 1
            results.append(proc.returncode)
    if any(code not in (0, 77) for code in results):
        return 1
    return 77 if 77 in results else 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    raise SystemExit(main())
