# Leonida — Guia rápido (`pt-PT`)

> [!NOTE]
> A documentação em inglês ([`README.md`](../../../README.md), [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) e
> [`docs/README.md`](../../README.md)) é a **referência oficial**. Este guia reúne apenas instruções estáveis de
> configuração e compilação (sincronizado com o commit `867d922f`). Para o estado diário do projeto, consulte o
> [`README.md`](../../../README.md).

## O que é o Leonida

O Leonida é um projeto de recompilação estática do Grand Theft Auto VI (Xbox Series) para correr nativamente no
Windows, construído **no seu PC a partir da sua própria cópia**. Os programas RDNA2 da consola são traduzidos para
SPIR-V (Vulkan), os recursos são lidos diretamente do seu pacote e o software de sistema da consola é
reimplementado em *shims*. Tudo isto acontece **no seu PC, com a sua própria cópia do jogo**.

> [!IMPORTANT]
> Este repositório **não contém o jogo, chaves nem executável**, e não explica como os obter. Não descriptografa
> nem contorna proteções. Tudo o que a compilação gera (shaders traduzidos, recursos extraídos, a pasta de
> execução e os seus saves) vem da sua cópia e **deve ficar apenas no seu PC**. Nunca anexe nem partilhe esses
> ficheiros em issues ou pull requests (veja [`CONTRIBUTING.md`](../../../CONTRIBUTING.md)).

Ainda não é possível jogar o jogo em nenhuma plataforma; consulte a secção *Project status* do README inglês.

## Pré-requisitos

- **A sua própria cópia** do pacote Xbox Series, versão `1.0.2.1` (pasta com `MicrosoftGame.Config` e os ficheiros
  `.rpf` legíveis em disco).
- **Windows 10/11 x64**, com cerca de **400 GB** livres para ficheiros de trabalho. Uma única exportação de
  recursos ocupou cerca de 337 GB antes da etapa de áudio. `--work` permite colocá-los noutra unidade.
- [Python 3.12+](https://www.python.org/downloads/) (marque *“Add python.exe to PATH”*).
- [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0).
- [Vulkan SDK](https://vulkan.lunarg.com/) (`spirv-as` e `spirv-val`).
- [LLVM 21+](https://github.com/llvm/llvm-project/releases) com o **alvo AMDGPU** para `llvm-mc`. O instalador
  oficial do LLVM para Windows não inclui esse alvo. No Windows, `scripts\build_llvm_mc.ps1` compila um
  `llvm-mc.exe` nativo com o alvo e define `LLVM_MC` (demora cerca de 20–40 minutos e precisa do git e das
  ferramentas C++ do Visual Studio). Outra opção é instalar `llvm-21` no WSL e definir
  `LLVM_MC="wsl -d Ubuntu -e /usr/lib/llvm-21/bin/llvm-mc"`. No Linux, basta ter `llvm-mc` no `PATH`.
  Consulte [`docs/design.md`](../../design.md#disassembling-shaders).
- **Visual Studio 2022** com *Desenvolvimento para desktop com C++* e **Windows SDK 10.0.26100 ou mais recente**
  (a etapa `runtime` usa assinaturas de root 1.2).
- [ffmpeg](https://ffmpeg.org/download.html) no `PATH` *(opcional: descodifica streams XMA2; obrigatório com
  `--wav`)*.

**Este PC chega?** O [Can I Run Leonida](https://assttro.github.io/can-i-run-leonida/) é um verificador leve no
navegador que compara o teu PC com perfis de referência. É uma estimativa aproximada, não um benchmark.

**Pouco espaço em disco:** os ~400 GB são para a compilação completa. Para *executar* o port,
`--stages check,shaders,runtime` precisa de cerca de 30 GB; a etapa `assets` (~370 GB) serve apenas para as
ferramentas de extração e `--wav` acrescenta várias vezes o tamanho do Ogg Opus. `--work` e `--out` podem ficar
noutra unidade. O runtime mapeia o pacote no local, pelo que uma unidade só de leitura exige
`gta6rt --overlay <pasta gravável>` — `--overlay` é uma opção do `gta6rt`, não do `make_game.py`. Mantenha os
ficheiros `.s` em `work/shaders/dxbc/disasm`: a execução seguinte compara-os com `disasm.done` e refaz os que
faltam. Uma pasta `disasm` que seja uma ligação simbólica é recusada quando a etapa precisa de a limpar.

## Como compilar

### Modo simples (Windows)

1. Instale os pré-requisitos acima.
2. Clone o repositório **com o submódulo**:
   ```powershell
   git clone --recursive https://github.com/ubiquitarianism/leonida
   ```
   Um *Download ZIP* do GitHub não inclui `third_party/rpf9-explorer`.
3. Abra a pasta `leonida` e faça duplo clique em **`Build.bat`**.

O script procura a sua cópia do jogo em Downloads, Ambiente de Trabalho, Documentos (incluindo OneDrive), Games,
pasta pessoal e raiz de cada unidade, até dois níveis. Reconhece a versão por alguns tamanhos de ficheiro, sem
ler nada de grande. Se encontrar mais do que uma cópia, pergunta qual usar; se não encontrar nenhuma, pede o
caminho. Antes de começar, verifica os pré-requisitos e o espaço livre e apresenta tudo o que falta de uma vez.
O resultado vai para `Documents\Leonida`. Uma compilação interrompida continua onde ficou.

### Pelo terminal

```powershell
python -I make_game.py --find                          # lista as cópias encontradas neste PC
python -I make_game.py --pick                          # escolhe a pasta do jogo numa janela de diálogo
python -I make_game.py --check                         # verifica a cópia sem compilar
python -I make_game.py --check --full-check            # compara também o SHA-256 de cada ficheiro (lento)
python -I make_game.py --game "D:\caminho\do\pacote" --out "D:\Games\Leonida" --work "E:\leonida-work"
python -I make_game.py --stages shaders,assets         # executa apenas as etapas escolhidas
python -I make_game.py --game "D:\caminho\do\pacote" --wav   # descodifica também Opus para PCM WAV (grande)
python -I make_game.py --wave64-emulate off            # auto (predefinição), on ou off; veja abaixo
```

As etapas executam-se nesta ordem: `check` primeiro; depois `shaders`, `assets` e `runtime` em conjunto quando
selecionadas; `pipelines` começa quando os shaders traduzidos estão prontos e cria cada um deles no controlador
GPU, guardando a cache em `pipeline_cache.bin` dentro de `--out` (o runtime ainda não a carrega). A tradução de
shaders é a etapa CPU mais longa e usa metade dos processadores por predefinição (`--jobs`). Num terminal
interativo, um ecrã de configuração permite ajustar o número de processos e a sobreposição de etapas, com uma
vista de logs por etapa (`1`–`4` / `Tab`); use `--no-tui` (ou redirecione a saída) para o console simples.
Os logs completos ficam em `work/logs/`.

- **`--wave64-emulate`**: traduz shaders compute e mesh wave64 para serem exatos em qualquer tamanho de subgrupo.
  NVIDIA e Intel têm subgrupos de 32 lanes e precisam disso. O modo `auto` consulta `vulkaninfo` e ativa a
  emulação se alguma GPU não tiver subgrupos de 64 lanes ou o tamanho for desconhecido; verifica também se a GPU
  do runtime suporta compute e mesh em 64 lanes (com `--mesh-compute`, basta compute). No controlador Windows da
  AMD, `auto` usa wave64 nativo em RDNA1/RDNA2 quando a GPU do runtime o suporta, mesmo que outra GPU seja de
  32 lanes ou `vulkaninfo` não esteja disponível. O modo escolhido fica guardado junto dos shaders; alterá-lo
  obriga a traduzir de novo. Veja *Wave64 emulation* em [`docs/design.md`](../../design.md).
- **Flags de fallback do tradutor**: `--no-float-controls` (remove `SignedZeroInfNanPreserve`),
  `--bary-fallback` (lê baricêntricas de um input interpolado sem `VK_KHR_fragment_shader_barycentric`),
  `--texel-offset-fallback` (incorpora offsets `image_sample*_o` na coordenada sem `VK_KHR_maintenance8`) e
  `--mesh-compute` (mesh shaders como compute sem `VK_EXT_mesh_shader`). São repassadas ao tradutor, guardadas
  com os shaders e alterá-las retraduz os shaders. Cada uma está documentada em
  [`docs/design.md`](../../design.md).
- **`--wav`**: desativado por predefinição porque WAV PCM de 16 bits é várias vezes maior que Ogg Opus. Depois
  da etapa de áudio, descodifica os streams para `work\assets\audio\wav`; ao retomar, os ficheiros já existentes
  são ignorados.

### Diagnóstico do Vulkan no Linux

Com CMake 3.24+, um compilador C++20, cabeçalhos de desenvolvimento e o carregador Vulkan instalados:

```sh
cmake -S runtime -B runtime/build/linux
cmake --build runtime/build/linux --parallel
./runtime/build/linux/vk_pipeline_check /caminho/para/spirv
```

Passe a pasta que contém `index.json` e os ficheiros SPIR-V traduzidos. No Linux, `make_game.py` também corre;
a etapa `pipelines` usa o `vk_pipeline_check` acima e compila-o se ainda não existir.

### Runtime no Linux com Wine ou Proton

O loader é um programa Windows e corre sob Wine ou Proton. No Linux, a etapa `runtime` compila-o de forma
cruzada para `runtime/build/windows` com clang-cl e lld-link contra o CRT do MSVC e o Windows SDK que o
[xwin](https://github.com/Jake-Shadle/xwin) organiza em `XWIN_DIR` ou `~/.local/share/xwin-sdk`. Sem isso, a
etapa fica fora da execução predefinida. Também pode reutilizar um build Windows existente:

```sh
python3 -I make_game.py --runtime-build ~/Downloads/runtime-windows
```

Em ambos os casos, o script copia `gta6rt.exe` e `gta6rt_tls_host.dll` para `--out` e cria `gta6rt.sh`, que
inicia o runtime com `wine` (ou com o Wine indicado por `WINE`). Use `<out>/gta6rt.sh --dry-run` para inspecionar
as importações, `--init` para verificar callbacks TLS e inicialização de DLL, e `--run` para o arranque.
Com `LEONIDA_D3D12=vulkan`, o runtime encontra os shaders em `<work>/shaders/spirv` através de `shader_path.txt`;
`LEONIDA_SHADERS` permite escolher outra pasta. Consulte
[`docs/testing.md`](../../testing.md#the-windows-runtime-on-linux-under-wine-or-proton).

## Como funciona

| Parte | O que faz | Onde |
|---|---|---|
| Verificação de versão | Nome, tamanho e SHA-256 de cada ficheiro do pacote para cada versão suportada | `game/`, `make_game.py` |
| Pipeline de shaders | Extrai contentores DXBC sem a tabela de conteúdos encriptada; desempacota o programa RDNA2 `XBCH` → `FSSM`; desmonta com LLVM (`gfx1013`) e traduz para SPIR-V | `tools/shader_carve.py`, `fssm_unpack.py`, `shader_disasm.py`, `rdna2spirv.py` |
| Exportação de recursos | Encontra recursos validados em arquivos RPF9 e nos pools `.ttpf` soltos | `tools/rpf9dump` |
| Runtime | Mapeia a imagem PE, resolve importações para *shims* (GDK, XMem, `d3d12_xs`) | `runtime/` |
| Ferramentas de investigação | Inspeção PE/RTTI, extração, sondas de mosaicos, harnesses GPU | `tools/`, `runtime/test/` |
| Painel de progresso | Uma página offline com o que passa e o que bloqueia o próximo passo, gerada a partir de testes e das próprias tabelas da documentação | `tools/progress_dashboard.py` |

O contrato do tradutor e todas as notas de formato estão em [`docs/design.md`](../../design.md); as notas de
formato, recursos, runtime e validação estão indexadas em [`docs/README.md`](../../README.md). O plano D3D12
está em [`docs/d3d12_vulkan.md`](../../d3d12_vulkan.md), os formatos de áudio em
[`docs/audio.md`](../../audio.md) e as referências relacionadas em
[`docs/related_projects.md`](../../related_projects.md).

## Problemas comuns

- **Usou o botão “Download ZIP” do GitHub**: o ZIP não inclui o submódulo `third_party/rpf9-explorer`. Use
  `git clone --recursive` ou `git submodule update --init --recursive`.
- **`llvm-mc` recusa `gfx1013` / AMDGPU**: está provavelmente a usar o instalador oficial do LLVM para Windows.
  Siga uma das opções de `LLVM_MC` acima.
- **Erro `D3D12_ROOT_SIGNATURE_DESC2` ao compilar o runtime**: atualize o Windows SDK para `10.0.26100` ou
  mais recente, apague a pasta de build e reconfigure.
- **Falta de espaço na unidade `C:`**: passe `--work "D:\leonida-work"` ou use as etapas reduzidas
  `--stages check,shaders,runtime`.

## Como contribuir e executar testes GPU

1. Consulte os PRs abertos e a [issue de coordenação][coord] para evitar trabalho duplicado. Reserve a tarefa
   com `claiming: <ID>` (ou com uma descrição de uma linha, se ainda não houver ID).
2. Execute os testes relevantes com entradas sintéticas. Por exemplo, na raiz do repositório:
   `python -I tests/test_manifest_verifier.py`.
3. No PR, indique o ID da tarefa, o que testou e a GPU/controlador se executou testes GPU. Siga as regras em
   [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) e indique o uso de IA conforme
   [`AGENTS.md`](../../../AGENTS.md).

O [guia de testes GPU](../../testing.md) explica como executar os testes sem ficheiros do jogo. Publique os
resultados como um pull request que adiciona um ficheiro a [gpu-test-results/](../../../gpu-test-results).

[coord]: https://github.com/ubiquitarianism/leonida/issues?q=is%3Aissue%20is%3Aopen%20in%3Atitle%20%22Coordinating%20who%20works%20on%20what%22
