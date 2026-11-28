# Leonida — Guia rápido (`pt-BR`)

> [!NOTE]
> A documentação em inglês ([`README.md`](../../../README.md), [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) e
> [`docs/README.md`](../../README.md)) é a **referência oficial**. Este guia reúne apenas instruções estáveis de
> configuração, compilação e execução (sincronizado com o commit `b1619e0a`). Para o status atualizado diariamente e
> notas técnicas detalhadas, consulte o [`README.md`](../../../README.md).

## O que é o Leonida

O Leonida é um projeto de recompilação estática do **Grand Theft Auto VI (Xbox Series)** para rodar nativamente no
Windows, construído **no seu próprio PC a partir da sua própria cópia**. Como a CPU do Xbox Series é x86-64, o
código do jogo é carregado e executado nativamente enquanto o software de sistema do console é reimplementado. Os
shaders RDNA2 pré-compilados são traduzidos para o backend gráfico do host e os modelos, texturas e áudios são lidos
diretamente do seu pacote.

> [!IMPORTANT]
> Este repositório **não contém o jogo, chaves nem executável**, e não explica como obtê-los. Você precisa da sua
> própria cópia com os arquivos já legíveis em disco. Nada aqui descriptografa ou contorna proteções. Tudo o que o
> build gera (shaders traduzidos, assets extraídos, pasta do runtime e seus saves) vem da sua cópia e **deve ficar
> apenas na sua máquina** — nunca anexe nem compartilhe esses arquivos em issues ou pull requests (veja
> [`CONTRIBUTING.md`](../../../CONTRIBUTING.md)).

> [!NOTE]
> **Continuação do [projeto Leonida de NotRexed](https://github.com/ProjectLeonida/Leonida)**, mantendo todo o
> histórico de commits. Comunidade: [Discord](https://discord.gg/ETf4qj5acN) · [Telegram](https://t.me/mandyport).

O projeto mantém **dois runtimes**:

- **Runtime Direct3D 12 ([`d3d12/`](../../../d3d12/README.md)) — jogável no mundo aberto no Windows (NVIDIA e AMD):**
  executável `leonida.exe` (derivado do VIctory) iniciado pelo **`Launcher.bat`** ou por `scripts/play-d3d12.ps1`.
  Carrega um save no mundo aberto com controle do personagem (controle Xbox/PlayStation ou teclado e mouse), som, HUD,
  mapa, menu de pausa, DLSS opcional e menus internos (`F1` configurações, `F3` métricas, `F4` menu).
- **Runtime recompilado com camada D3D12 → Vulkan ([`runtime/`](../../../runtime/README.md)) — ainda não jogável:**
  em Windows nativo e Linux/Proton, já passa pelo vídeo de abertura, telas de primeiro uso e carregamento da história
  até a fase de mundo (sem quadro do mundo verificado ainda). Veja [primeiro uso no Windows](../../first_run_ui.md) e
  o [relatório Linux/Proton](../../linux_proton.md).

## Plataformas

| Plataforma | O que funciona |
|---|---|
| Windows | Tudo: o runtime Direct3D 12 jogável (`d3d12/`), `make_game.py`, as ferramentas e o runtime recompilado (`gta6rt` + camada D3D12 → Vulkan) com seus testes. |
| Linux | O tradutor, as ferramentas de assets e os testes Vulkan de GPU rodam nativamente (RADV e NVIDIA). O runtime `d3d12/` roda via `scripts/play-linux.sh` (com VKD3D-Proton), e o runtime `runtime/` é compilado de forma cruzada (`clang-cl` + `xwin`) e roda sob Wine ou Proton. Use `./Build.sh`; pacotes e passos em [docs/linux.md](../../linux.md). |
| macOS (Apple Silicon) | As ferramentas Python e os testes de GPU Vulkan rodam nativamente em MoltenVK (Homebrew) ou KosmicKrisp; o runtime Windows roda via Wine (CrossOver, FEX ou Rosetta). Limites conhecidos: sem estágio geometry nem `VK_EXT_mesh_shader`, subgrupos de 32 lanes (wave64 emulado) e particularidades de ponto flutuante no MoltenVK ([docs/testing.md](../../testing.md)). |
| AMD RDNA1 (RX 5000) | Alvo suportado: subgrupos de 64 lanes executam shaders wave64 nativamente. No RADV, shaders de pixel sem baricêntricas de fragmento têm diferença documentada em [docs/testing.md](../../testing.md). |

## Iniciando o jogo rapidamente (Launcher e runtime D3D12)

Se o objetivo é **jogar no Windows com o runtime Direct3D 12 (`d3d12/`)**:

1. Instale **Windows 10/11 x64**, CPU com AVX2, GPU Direct3D 12 com Shader Model 6.6 e mesh shaders,
   [Python 3.12+](https://www.python.org/downloads/), Rust e as ferramentas de build C++ do Visual Studio 2022.
2. Clone o repositório **com o submódulo**:

   ```powershell
   git clone --recursive https://github.com/ProjectLeonida/mandy
   cd mandy
   ```

3. *(Opcional, NVIDIA)* Para ativar **DLSS e DLSS Frame Generation**, execute uma vez `d3d12\Get-NVIDIA-DLSS.cmd`
   (baixa ~70 MB diretamente do GitHub da NVIDIA para `d3d12\nvidia-sdk\`). Sem isso o jogo ainda roda, apenas
   escalonando a imagem na tela.
4. Dê duplo clique em **`Launcher.bat`** (no Linux: `python3 tools/leonida_launcher.py`, requer `python3-tk`), informe
   a pasta da sua cópia em *Settings* e clique em **PLAY**. Veja [`docs/launcher.md`](../../launcher.md) e
   [`d3d12/README.md`](../../../d3d12/README.md).

Pelo terminal PowerShell, o launcher chama [`scripts/play-d3d12.ps1`](../../../scripts/play-d3d12.ps1) (que compila
`d3d12/` automaticamente na primeira vez e pré-compila shaders antes de abrir o jogo):

- **Padrão (Series X):** `powershell -ExecutionPolicy Bypass -File scripts\play-d3d12.ps1 -Game "D:\caminho\do\pacote"`
- **Perfil Series S com DLSS:** `powershell -ExecutionPolicy Bypass -File scripts\play-d3d12.ps1 -Game "D:\caminho\do\pacote" -SeriesS -Dlss quality`
- **Conferir opções sem compilar nem iniciar:** `powershell -ExecutionPolicy Bypass -File scripts\play-d3d12.ps1 -Game "D:\caminho\do\pacote" -DryRun`

Opções úteis: `-SeriesS` (perfil Series S, mais leve que o padrão Series X), `-Dlss quality|balanced|performance|ultra`,
`-FrameGen 2`, `-RayTracing` (mais lento; compila mais pipelines na primeira visita), `-NoPrecompile` (pula o
pré-aquecimento de shaders) e `-Log` (grava o log em `%LOCALAPPDATA%\Leonida\logs`).

## Pré-requisitos do pipeline completo (`make_game.py` e runtime Vulkan)

- **Sua própria cópia** do pacote Xbox Series, versão `1.0.2.1` (pasta com `MicrosoftGame.Config` e os arquivos `.rpf`
  legíveis em disco).
- **Windows 10/11 x64** com cerca de **400 GB livres** para o build completo (400 GiB no Windows Explorer ≈ 430 GB
  decimais; só a exportação de assets mediu ~337 GB antes do áudio, em ~2 h 35 min) ou cerca de **30 GB livres**
  para preparar apenas o runtime e os shaders (`--stages check,shaders,runtime`).
- [Python 3.12+](https://www.python.org/downloads/) (marque *"Add python.exe to PATH"*).
- [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0).
- [Vulkan SDK](https://vulkan.lunarg.com/) (`spirv-as`, `spirv-val` e `vulkaninfo`).
- [LLVM 21+](https://github.com/llvm/llvm-project/releases) com o **target AMDGPU** (`llvm-mc`):
  - O instalador oficial do LLVM para Windows **não** inclui o target AMDGPU.
  - **Forma mais rápida no Windows (conda):** instale com `conda install -c conda-forge llvm-tools` (o pacote do
    conda-forge para Windows inclui o target AMDGPU) e aponte `LLVM_MC` para o caminho completo do `llvm-mc.exe`
    (`where.exe llvm-mc` mostra o caminho no ambiente ativo; no PowerShell, `where` sem `.exe` é `Where-Object`).
  - **Compilação nativa no Windows (sem WSL):** rode na raiz do repositório (leva cerca de 20–40 minutos, requer `git`
    e as ferramentas C++ do Visual Studio e grava `LLVM_MC` para o usuário):

    ```powershell
    powershell -ExecutionPolicy Bypass -File scripts\build_llvm_mc.ps1
    ```

  - **Via WSL:** instale `llvm-21` no Ubuntu e defina `LLVM_MC` na sessão do PowerShell antes de rodar `make_game.py`:

    ```powershell
    $env:LLVM_MC = 'wsl -d Ubuntu -e /usr/lib/llvm-21/bin/llvm-mc'
    ```

    Veja [`docs/design.md`](../../design.md#disassembling-shaders) para mais detalhes.
  - **No Linux:** basta `llvm-mc` no `PATH` ou uma instalação versionada da distribuição, como `llvm-21` no Ubuntu
    (`llvm-mc-21`, `/usr/lib/llvm-21/bin/llvm-mc`).
- **Visual Studio 2022** com *Desenvolvimento para desktop com C++* e **Windows SDK `10.0.26100` ou mais recente**
  (o estágio `runtime` usa root signatures versão 1.2).
- [ffmpeg](https://ffmpeg.org/download.html) no `PATH` *(opcional: decodifica streams XMA2 e é obrigatório com `--wav`)*.

**Este PC aguenta?** O verificador leve no navegador [Can I Run Leonida](https://assttro.github.io/can-i-run-leonida/)
compara sua máquina com perfis de referência (estimativa aproximada, não é um benchmark).

## Como compilar o port (`Build.bat` / `Build.sh` / `make_game.py`)

### Modo simples

1. Instale os pré-requisitos acima.
2. Baixe o projeto **com o submódulo**:

   ```powershell
   git clone --recursive https://github.com/ProjectLeonida/mandy
   ```

   O botão *"Download ZIP"* do GitHub omite `third_party/rpf9-explorer` (o build tenta buscá-lo automaticamente se
   `git` estiver instalado; caso contrário, informa o que fazer).
3. No Windows, abra a pasta clonada e dê duplo clique em **`Build.bat`**. No Linux ou macOS, execute **`./Build.sh`**
   no terminal (veja [docs/linux.md](../../linux.md)).

O script localiza automaticamente sua cópia (Downloads, Desktop, Documentos — inclusive pastas redirecionadas como
OneDrive —, Games, pasta do usuário e raiz dos discos até dois níveis), valida pré-requisitos e espaço livre de uma
só vez e grava o port em `Documents\Leonida`. Se o build for interrompido, execute novamente para continuar de onde
parou.

### Espaço em disco e distribuição entre unidades

O build usa três pastas, e cada uma pode ficar em um disco diferente:

| Pasta | Flag | O que contém | Tamanho | Lida ao jogar |
|---|---|---|---|---|
| Seu pacote | `--game` | Sua cópia do jogo (somente leitura, nunca modificada) | Original | Sim, no local (`game_path.txt`) |
| Trabalho | `--work` (padrão `<out>\work`) | `shaders` (extraídos, desmontados e traduzidos) e `assets` (exportações) | ~30 GB + ~370 GB | Apenas `shaders\spirv` (`shader_path.txt`) |
| Port | `--out` (padrão `Documents\Leonida`) | `gta6rt.exe`, DLLs, arquivos de caminho, cache de pipelines e `overlay` | Pequeno | Sim |

A checagem de espaço livre verifica apenas o disco de `--work`. Nada em `<work>\assets` é lido durante o jogo; com um
SSD pequeno, mantenha o runtime e os shaders no SSD (~30 GB) e coloque as exportações de assets em um HD separado em
duas execuções:

```powershell
# No SSD: pacote, port e shaders traduzidos (~30 GB de trabalho)
python -I make_game.py --game "S:\Games\package" --out "S:\Games\Leonida" --work "S:\Games\Leonida\work" --stages check,shaders,runtime,pipelines
# No HD: exportação de assets (~370 GB), usada apenas pelas ferramentas de assets
python -I make_game.py --game "S:\Games\package" --out "S:\Games\Leonida" --work "H:\leonida-assets" --stages check,assets
```

Se a unidade do pacote for somente leitura, passe `--overlay <pasta gravável>` ao iniciar o `gta6rt` (`--overlay` é
opção do `gta6rt`, não do `make_game.py`). Mantenha os arquivos `.s` em `work/shaders/dxbc/disasm`: em execuções
seguintes, o build compara a pasta com a lista em `disasm.done` e refaz a desmontagem dos ausentes (e retraduz todos
os shaders); uma pasta `disasm` que seja symlink é recusada.

### Pelo terminal (`make_game.py`)

```powershell
python -I make_game.py --find                          # lista as cópias encontradas neste PC
python -I make_game.py --pick                          # escolhe a pasta do jogo em uma janela de diálogo
python -I make_game.py --check                         # verifica os arquivos do jogo sem compilar
python -I make_game.py --check --full-check            # também compara o SHA-256 de cada arquivo (lento)
python -I make_game.py --game "D:\caminho\do\pacote" --out "D:\Games\Leonida" --work "E:\leonida-work"
python -I make_game.py --stages shaders,assets         # executa apenas os estágios escolhidos
python -I make_game.py --game "D:\caminho\do\pacote" --wav  # também decodifica Ogg Opus para PCM WAV (grande)
python -I make_game.py --wave64-emulate off            # auto (padrão), on ou off; veja abaixo
```

**Ordem dos estágios e interface:** `check` roda sempre primeiro; em seguida, quando selecionados, `shaders`, `assets`
e `runtime` rodam em paralelo (são independentes). `pipelines` inicia assim que os shaders traduzidos estiverem
prontos (e após o rebuild de `vk_pipeline_check`, quando `runtime` está incluído), gravando `pipeline_cache.bin` em
`--out`. Em terminal interativo, a tela LEONIDA permite configurar `--jobs` e alternar logs por estágio (`1`–`4` /
`Tab`); logs completos ficam em `work/logs/`. Use `--no-tui` para saída simples no console.

- **`--wave64-emulate`**: traduz shaders compute/mesh wave64 para funcionarem de forma exata em GPUs de 32 lanes
  (NVIDIA e Intel). O padrão `auto` consulta `vulkaninfo` e ativa emulação se alguma GPU não tiver subgrupos de 64
  lanes ou se o tamanho for desconhecido (com `--mesh-compute`, exige apenas suporte em compute). No driver Windows da
  AMD, `auto` usa wave64 nativo em RDNA1 e RDNA2 quando a GPU do runtime suporta (mesmo com outra GPU de 32 lanes ou
  sem `vulkaninfo`), para evitar crashes do driver em código LDS emulado. Use `on` ou `off` para forçar; trocar o
  modo retraduz os shaders.
- **Flags de fallback do tradutor**: `--no-float-controls` (omite `SignedZeroInfNanPreserve`), `--bary-fallback` (lê
  baricêntricas por input interpolado sem `VK_KHR_fragment_shader_barycentric`), `--texel-offset-fallback` (incorpora
  offsets `image_sample*_o` na coordenada sem `VK_KHR_maintenance8`) e `--mesh-compute` (traduz mesh shaders como
  compute sem `VK_EXT_mesh_shader`; ativado automaticamente no driver Vulkan Windows da Intel salvo se `--native-mesh`
  for passado). Todas ficam salvas junto aos shaders e alterá-las dispara nova tradução ([`docs/design.md`](../../design.md)).
- **`--jobs` e `--wav`**: a tradução de shaders é a etapa de CPU mais longa e usa metade das threads lógicas por
  padrão (`--jobs` ajusta o limite). `--wav` fica desligado por padrão porque WAV PCM de 16 bits é várias vezes maior
  que Ogg Opus; quando ativado, decodifica para `work\assets\audio\wav` pulando arquivos já existentes.
- **Atualizações automáticas de cache**: o build registra a versão do `llvm-mc`, os arquivos do tradutor e a revisão do
  decodificador (`rpf9-explorer` + `tools/rpf9dump`), refazendo automaticamente apenas as etapas afetadas após uma
  atualização.

### Diagnóstico Vulkan e runtime no Linux (Wine / Proton)

Para compilar o verificador de pipelines Vulkan nativo no Linux (requer CMake 3.24+, compilador C++20 e cabeçalhos/loader
Vulkan):

```sh
cmake -S runtime -B runtime/build/linux
cmake --build runtime/build/linux --parallel
./runtime/build/linux/vk_pipeline_check /caminho/para/spirv
```

Para executar o loader `gta6rt.exe` no Linux (sob Wine ou Proton), `make_game.py` pode compilá-lo de forma cruzada em
`runtime/build/windows` com `clang-cl` + `lld-link` (LLVM 19+, exceto 22.1.7 e 22.1.8 — veja
[problemas conhecidos](../../testing.md#known-problems)) e o SDK Windows instalado via
[xwin](https://github.com/Jake-Shadle/xwin) em `XWIN_DIR` ou `~/.local/share/xwin-sdk` (instruções em
[docs/testing.md](../../testing.md#the-windows-runtime-on-linux-under-wine-or-proton)). Também é possível reutilizar
um build Windows pronto (como o artefato `runtime-windows` do workflow Runtime):

```sh
python3 -I make_game.py --runtime-build ~/Downloads/runtime-windows
```

O script copia `gta6rt.exe` e `gta6rt_tls_host.dll` para `--out` e gera `gta6rt.sh` (usa `wine` por padrão, ou o
binário indicado em `WINE` — no Proton, seu `files/bin/wine`). Para inspecionar e iniciar:

- `<out>/gta6rt.sh --dry-run` (inspeciona imports), `<out>/gta6rt.sh --init` (testa TLS e inicialização de DLLs) e
  `<out>/gta6rt.sh --run` (execução usando `game_path.txt`, `overlay` e `LEONIDA_D3D12=vulkan`).
- Com `LEONIDA_D3D12=vulkan`, o runtime lê os shaders de `shader_path.txt` (ou da pasta definida em `LEONIDA_SHADERS`).
- Ao usar `tools/pe_inspect.py` no Linux, defina `WINEPREFIX` para o prefixo do loader (ou `~/.wine`) para classificar
  DLLs de sistema corretamente.

## Problemas comuns

- **Baixou pelo botão "Download ZIP" do GitHub**: o ZIP não inclui `third_party/rpf9-explorer`. Use
  `git clone --recursive` ou execute `git submodule update --init --recursive` dentro da pasta clonada.
- **`llvm-mc` recusa `gfx1013` / AMDGPU**: o instalador oficial do LLVM para Windows não traz esse target. Use o pacote
  `llvm-tools` do conda-forge, `scripts\build_llvm_mc.ps1` ou `llvm-21` no WSL conforme descrito acima.
- **Erro de `D3D12_ROOT_SIGNATURE_DESC2` ao compilar o runtime**: atualize o Windows SDK para `10.0.26100` ou mais
  recente (Visual Studio Installer ou `winget install Microsoft.WindowsSDK.10.0.26100`), apague a pasta de build e
  reconfigure.
- **Falha por falta de espaço no drive `C:`**: mova o diretório de trabalho com `--work "D:\leonida-work"` ou use o
  esquema de duas unidades em [Espaço em disco e distribuição entre unidades](#espaço-em-disco-e-distribuição-entre-unidades).
- **DLSS / Frame Generation indisponíveis no runtime D3D12 (`d3d12/`)**: execute `d3d12\Get-NVIDIA-DLSS.cmd` uma vez
  para baixar os binários oficiais da NVIDIA localmente.

## Como contribuir e rodar os testes de GPU

1. Consulte os PRs abertos e a [issue de coordenação][coord] para evitar trabalho duplicado. Reserve a tarefa com
   `claiming: <ID>` (ou uma descrição de uma linha se ainda não houver ID) e publique `done: <ID> -> #<PR>` ao abrir o
   pull request.
2. Rode os testes relevantes com entradas sintéticas (sem dados do jogo) — por exemplo,
   `python -I tests/test_manifest_verifier.py` na raiz do repositório.
3. No PR, inclua o ID da tarefa, descreva o que foi testado e informe GPU e driver se rodou testes em GPU. Siga
   [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) e indique assistência de IA conforme [`AGENTS.md`](../../../AGENTS.md).

O [guia de testes em GPU](../../testing.md) explica como executar os testes sem arquivos do jogo. Para enviar seus
resultados, abra um pull request adicionando um arquivo em [`gpu-test-results/`](../../../gpu-test-results) no formato
dos relatórios existentes.

[coord]: https://github.com/ProjectLeonida/mandy/issues?q=is%3Aissue%20is%3Aopen%20in%3Atitle%20%22Coordinating%20who%20works%20on%20what%22
