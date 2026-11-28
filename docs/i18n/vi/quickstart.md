# Leonida — Hướng dẫn bắt đầu nhanh (`vi`)

> [!NOTE]
> Tài liệu tiếng Anh ([`README.md`](../../../README.md), [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) và
> [`docs/README.md`](../../README.md)) **là bản có giá trị chính thức**. Hướng dẫn này chỉ gồm nội dung hướng
> dẫn ổn định về cài đặt và biên dịch (đồng bộ với commit `507029d1`). Trạng thái dự án thay đổi hằng ngày
> được ghi trong [`README.md`](../../../README.md).

## Leonida là gì

Leonida là dự án biên dịch lại tĩnh (static recompilation) của Grand Theft Auto VI (bản Xbox Series) để chạy
nguyên bản trên Windows. Nó được biên dịch **trên PC của bạn, từ bản sao trò chơi của chính bạn**. CPU của
Xbox Series là x86-64 giống PC, nên mã của trò chơi được nạp và chạy trực tiếp, còn phần mềm hệ thống của máy
console được cài đặt lại dưới dạng các shim. Các chương trình GPU RDNA2 biên dịch sẵn của console được dịch
sang SPIR-V (Vulkan), các tài nguyên được đọc trực tiếp từ gói của bạn.

> [!IMPORTANT]
> Kho này **không chứa trò chơi, khóa hay tệp thực thi**, và không hướng dẫn cách lấy chúng. Bạn cần bản sao
> của riêng mình, với các tệp đã ở dạng đọc được trên đĩa. Không có gì ở đây giải mã hay vượt qua bất kỳ biện
> pháp bảo vệ nào. Mọi thứ bản biên dịch tạo ra (shader đã dịch, tài nguyên đã trích xuất, thư mục làm việc,
> bản lưu) đều xuất phát từ bản sao của bạn và **phải ở lại trên máy của bạn**: đừng bao giờ đính kèm hay chia
> sẻ các tệp đó trong issue hay pull request (xem [`CONTRIBUTING.md`](../../../CONTRIBUTING.md)).

Đây là dự án tiếp nối [Leonida của NotRexed](https://github.com/ItsNotRexed/Leonida): tác giả gốc đã rời dự
án, và việc phát triển tiếp tục tại đây với toàn bộ lịch sử commit.

Dự án còn rất sớm: **trò chơi chưa khởi động được**.

## Nền tảng

Chưa nền tảng nào chơi được. Những gì chạy được hiện nay:

- **Windows**: mọi thứ (`make_game.py`, các công cụ, và runtime `gta6rt` cùng lớp D3D12 → Vulkan) kèm các
  kiểm thử của nó.
- **Linux**: trình dịch, các công cụ tài nguyên và công cụ kiểm thử GPU Vulkan biên dịch và chạy nguyên bản
  (đã kiểm thử trên RADV và NVIDIA). Runtime là mã Windows: nó được biên dịch chéo bằng clang-cl và xwin, và
  các kiểm thử của nó chạy dưới Wine hoặc Proton. Trò chơi dự kiến cũng chạy theo cách đó (#217).
  [`docs/linux.md`](../../linux.md) có các gói cần cài và các bước thực hiện.
- **macOS (Apple Silicon)**: các công cụ Python và công cụ kiểm thử Vulkan chạy nguyên bản, trên MoltenVK
  (Homebrew) hoặc KosmicKrisp. Runtime chạy dưới Wine, như trên Linux. Hạn chế đã biết: không có tầng
  geometry và không có `VK_EXT_mesh_shader`, subgroup rộng 32 (shader wave64 chạy giả lập), và cách xử lý số
  thực dấu phẩy động của MoltenVK; [`docs/testing.md`](../../testing.md) liệt kê chi tiết.
- **AMD RDNA1 (RX 5000)**: một đích được hỗ trợ, gần với RDNA2 của console; subgroup rộng 64 của nó chạy
  shader wave64 nguyên bản.

Kết quả kiểm thử GPU được nộp dưới dạng pull request thêm tệp vào thư mục `gpu-test-results/` của kho;
[hướng dẫn kiểm thử GPU](../../testing.md) giải thích cách chạy kiểm thử mà không cần tệp nào của trò chơi.

## Yêu cầu tiên quyết

- **Bản sao của riêng bạn** của gói Xbox Series, phiên bản `1.0.2.1`: thư mục chứa `MicrosoftGame.Config` và
  các tệp lưu trữ `.rpf`, ở dạng tệp đọc được trên đĩa.
- **Windows 10/11 x64**, với khoảng **400 GB trống** cho các tệp làm việc. Chúng có thể nằm trên ổ đĩa khác
  nhờ `--work`.
- [Python 3.12+](https://www.python.org/downloads/) (đánh dấu "Add python.exe to PATH" khi cài đặt).
- [SDK .NET 8](https://dotnet.microsoft.com/download/dotnet/8.0).
- [SDK Vulkan](https://vulkan.lunarg.com/) (`spirv-as`, `spirv-val` và `vulkaninfo`).
- [LLVM 21+](https://github.com/llvm/llvm-project/releases) với **đích AMDGPU** (`llvm-mc`):
  - Bản LLVM chính thức cho Windows **không gồm** đích AMDGPU.
  - Để biên dịch một `llvm-mc.exe` nguyên bản có đích này, chạy `.\scripts\build_llvm_mc.ps1` trong
    PowerShell từ thư mục gốc của kho. Script cần `git` và các công cụ C++ của Visual Studio. Nó đặt
    `LLVM_MC` cho người dùng của bạn: hãy mở một terminal mới để biến có hiệu lực. Mất khoảng 20 đến
    40 phút.
  - Nhanh hơn nếu bạn dùng [conda](https://docs.conda.io/): `conda install -c conda-forge llvm-tools` (bản
    Windows của nó gồm đích AMDGPU), rồi trỏ `LLVM_MC` tới đường dẫn đầy đủ của `llvm-mc.exe` đã cài
    (`where.exe llvm-mc` hiển thị đường dẫn sau khi môi trường được kích hoạt; trong PowerShell, `where`
    đứng một mình là bí danh của `Where-Object`).
  - Với WSL, cài `llvm-21` trong Ubuntu và đặt `LLVM_MC` thành toàn bộ lệnh, trong phiên PowerShell nơi bạn
    sẽ chạy `make_game.py`:

    ```powershell
    $env:LLVM_MC = 'wsl -d Ubuntu -e /usr/lib/llvm-21/bin/llvm-mc'
    ```

    Xem [`docs/design.md`](../../design.md#disassembling-shaders) để biết chi tiết.
  - Trên Linux, chỉ cần `llvm-mc` có trong `PATH`.
- **Visual Studio 2022** với workload *Desktop development with C++* và **Windows SDK `10.0.26100` trở
  lên**, cho giai đoạn `runtime` (runtime dùng root signature 1.2, mà các SDK cũ hơn không khai báo).
- [ffmpeg](https://ffmpeg.org/download.html) trong `PATH` *(tùy chọn: giải mã các luồng XMA2, và bắt buộc
  với `--wav`)*.

**PC này có đủ mạnh không?** [Can I Run Leonida](https://assttro.github.io/can-i-run-leonida/) là công cụ
kiểm tra nhẹ chạy trên trình duyệt, so sánh PC của bạn với các cấu hình tham chiếu. Đây chỉ là ước lượng
thô, không phải benchmark.

## Cách biên dịch

### Cách dễ nhất (Windows)

1. Cài đặt các yêu cầu tiên quyết ở trên.
2. Clone kho **kèm submodule của nó**:
   ```powershell
   git clone --recursive https://github.com/ubiquitarianism/leonida
   ```
   "Download ZIP" từ GitHub không gồm `third_party/rpf9-explorer`: quá trình biên dịch sẽ tự lấy nó nếu
   `git` đã được cài, và nếu không sẽ cho bạn biết cần làm gì.
3. Nhấp đúp vào **`Build.bat`**. Trên Linux hoặc macOS, thay vào đó chạy **`./Build.sh`** từ terminal.

Script tự tìm bản sao trò chơi của bạn: nó tìm trong Downloads, Desktop, Documents (kể cả nơi Windows cất
chúng, ví dụ trong OneDrive), Games, thư mục nhà của bạn và ở đầu mỗi ổ đĩa, sâu hai thư mục. Nó nhận ra
phiên bản từ kích thước của vài tệp, mà không đọc gì lớn. Nếu tìm thấy nhiều bản sao, nó hỏi bạn chọn bản
nào; nếu không thấy bản nào, nó yêu cầu bạn dán đường dẫn thư mục. Trước khi bắt đầu, nó kiểm tra các yêu
cầu tiên quyết và dung lượng trống, rồi liệt kê một lượt mọi thứ còn thiếu. Kết quả nằm trong
`Documents\Leonida`. Một bản biên dịch bị dừng giữa chừng sẽ tiếp tục từ chỗ đã dừng.

Thư mục trò chơi của bạn là thư mục chứa `MicrosoftGame.Config` và các tệp lưu trữ `.rpf`, ở dạng tệp
thường.

### Ít dung lượng đĩa

Con số khoảng 400 GB ở trên là cho bản biên dịch đầy đủ. Để *chạy* bản port, `--stages check,shaders,runtime`
cần khoảng 30 GB; giai đoạn `assets`, chiếm gần như toàn bộ phần còn lại (khoảng 370 GB), chỉ phục vụ các
công cụ tài nguyên, và `--wav` cộng thêm vài lần kích thước của Ogg Opus. `--work` và `--out` có thể nằm
trên ổ đĩa khác. Runtime ánh xạ gói tại chỗ: nếu ổ chứa gói chỉ đọc, hãy truyền
`gta6rt --overlay <thư mục ghi được>` (đây là tùy chọn của `gta6rt`, không phải của `make_game.py`). Đừng
xóa các tệp `.s` trong `work/shaders/dxbc/disasm`: lần chạy sau đối chiếu chúng với danh sách trong
`disasm.done` và dịch ngược lại những tệp còn thiếu, rồi dịch lại mọi shader.

### Từ terminal (`make_game.py`)

```powershell
python -I make_game.py --find                          # liệt kê các bản sao tìm thấy trên PC này
python -I make_game.py --pick                          # mở cửa sổ để chọn thư mục trò chơi
python -I make_game.py --check                         # kiểm tra bản sao của bạn, không biên dịch gì
python -I make_game.py --check --full-check            # kiểm tra cả SHA-256 của mọi tệp (chậm)
python -I make_game.py --game "D:\duong\dan\den\goi" --out "D:\Games\Leonida" --work "E:\leonida-work"
python -I make_game.py --stages shaders,assets         # chỉ một số giai đoạn
python -I make_game.py --game "D:\duong\dan\den\goi" --wav  # giải mã thêm Opus sang WAV PCM (rất lớn)
python -I make_game.py --wave64-emulate off            # auto (mặc định), on hoặc off; xem bên dưới
```

`check` luôn chạy đầu tiên; sau đó `shaders`, `assets` và `runtime` chạy cùng nhau khi được chọn (chúng
không phụ thuộc lẫn nhau). `pipelines` bắt đầu ngay khi các shader đã dịch sẵn sàng và, nếu lần chạy này
gồm `runtime`, khi giai đoạn đó đã biên dịch lại `vk_pipeline_check`: vì vậy nó có thể chạy trong lúc phần
còn lại của `assets` đang chạy. Giai đoạn `pipelines` tạo mỗi shader đã dịch trên driver GPU của bạn, kiểm
tra nó biên dịch được, báo cáo những shader nó phải bỏ qua và lưu bộ nhớ đệm pipeline vào
`pipeline_cache.bin` (trong `--out`); runtime chưa nạp bộ nhớ đệm đó.

- **`--wave64-emulate`**: quyết định các shader compute và mesh wave64 có được dịch để chính xác trên mọi
  kích thước subgroup hay không. GPU NVIDIA và Intel có subgroup rộng 32 và cần tính năng này. Giá trị mặc
  định, `auto`, hỏi `vulkaninfo` và bật giả lập nếu có GPU nào thiếu subgroup rộng 64 hoặc không rõ kích
  thước subgroup của nó. Với driver Windows của AMD, `auto` giữ wave64 nguyên bản trên RDNA1 và RDNA2 khi
  GPU của runtime cho phép, vì driver đó có thể sập trên mã giả lập (#857). Chế độ được lưu cạnh các shader
  đã dịch, và thay đổi nó sẽ dịch lại chúng.
- **Các cờ dự phòng** cho các thiết bị thiếu một tính năng Vulkan, cũng được lưu và dịch lại theo cách
  tương tự: `--no-float-controls`, `--bary-fallback` (không có `VK_KHR_fragment_shader_barycentric`),
  `--texel-offset-fallback` (không có `VK_KHR_maintenance8`) và `--mesh-compute` (không có
  `VK_EXT_mesh_shader`). Xem [`docs/design.md`](../../design.md).
- **Những gì được làm lại tự động**: bản biên dịch ghi lại lệnh và phiên bản của `llvm-mc` đã tạo bản dịch
  ngược, các nguồn của trình dịch và bộ giải mã tài nguyên (`third_party/rpf9-explorer` và
  `tools/rpf9dump`). Khi một trong số đó thay đổi, chỉ giai đoạn liên quan được làm lại (dịch ngược, dịch
  shader, hoặc xuất mô hình và texture).
- **`--work`**: chọn một ổ đĩa có ít nhất 400 GB trống. Dịch shader là bước tốn CPU nhất; mặc định nó dùng
  một nửa số luồng của CPU (điều chỉnh bằng `--jobs`).
- **`--wav`**: tắt theo mặc định, vì WAV PCM 16 bit lớn gấp nhiều lần Ogg Opus.
- **Linux và Wine/Proton**: `make_game.py` cũng chạy trên Linux. Giai đoạn `pipelines` dùng
  `vk_pipeline_check` (biên dịch bằng CMake trong `runtime/build/linux`, hoặc do chính script biên dịch
  nếu thiếu). Bộ nạp là chương trình Windows và chạy dưới Wine hoặc Proton: giai đoạn `runtime` biên dịch
  chéo nó bằng clang-cl và lld-link (LLVM 19+, trừ 22.1.7 và 22.1.8) cùng Windows SDK do
  [xwin](https://github.com/Jake-Shadle/xwin) chuẩn bị; xem [`docs/testing.md`](../../testing.md). Bạn cũng
  có thể truyền `--runtime-build <thư mục>` với một bản biên dịch Windows làm ở nơi khác (`gta6rt.exe` và
  `gta6rt_tls_host.dll`): trong cả hai trường hợp, script viết `gta6rt.sh`, khởi chạy `gta6rt.exe` bằng
  `wine`, hoặc bằng Wine do biến `WINE` chỉ định (với Proton, là `files/bin/wine` của nó).

## Sự cố thường gặp

- **Bạn đã dùng "Download ZIP" trên GitHub**: ZIP không chứa submodule `third_party/rpf9-explorer`. Hãy
  dùng `git clone --recursive`, hoặc chạy `git submodule update --init --recursive` trong thư mục của kho.
- **`llvm-mc` từ chối `gfx1013` hoặc đích AMDGPU**: có thể bạn đang dùng bản cài đặt LLVM chính thức cho
  Windows. Chạy `scripts\build_llvm_mc.ps1`, hoặc trỏ `LLVM_MC` tới `llvm-mc` của WSL (`llvm-21`).
- **CMake dừng ở "needs the Windows SDK 10.0.26100 or newer"**, hoặc MSVC báo `Desc_1_2` không phải thành
  viên của `D3D12_VERSIONED_ROOT_SIGNATURE_DESC`: cập nhật Windows SDK lên `10.0.26100` trở lên, qua Visual
  Studio Installer hoặc bằng `winget install Microsoft.WindowsSDK.10.0.26100`, rồi xóa `runtime/build` và
  cấu hình lại.
- **Hết chỗ trên `C:`**: truyền `--work "D:\leonida-work"` để đặt các tệp tạm trên một ổ đĩa khác.
- **Windows từ chối chạy các chương trình kiểm thử vừa biên dịch** ("An Application Control policy has
  blocked this file"): đó là Smart App Control của Windows 11. Xem
  [`docs/testing.md`](../../testing.md#known-problems).

## Đóng góp và chạy kiểm thử GPU

1. Xem các pull request đang mở và [issue điều phối][coord] để không làm trùng công việc đang diễn ra. Đặt
   chỗ cho công việc bằng `claiming: <ID>`, hoặc bằng một dòng mô tả nếu chưa có ID.
2. Chạy các kiểm thử liên quan, với dữ liệu đầu vào tổng hợp. Ví dụ, `python -I tests/test_manifest_verifier.py`
   từ thư mục gốc của kho.
3. Trong pull request, ghi ID của công việc, những gì bạn đã kiểm thử, cùng GPU và driver nếu bạn đã chạy
   kiểm thử GPU. Nêu rõ bạn có được AI hỗ trợ hay không. Các quy tắc nằm trong
   [`CONTRIBUTING.md`](../../../CONTRIBUTING.md).

[HƯỚNG DẪN KIỂM THỬ GPU](../../testing.md) giải thích cách chạy kiểm thử mà không cần tệp nào của trò
chơi. Kết quả được nộp dưới dạng pull request thêm tệp vào [`gpu-test-results/`](../../../gpu-test-results),
theo định dạng của các tệp đã có ở đó.

[coord]: https://github.com/ubiquitarianism/leonida/issues?q=is%3Aissue%20is%3Aopen%20in%3Atitle%20%22Coordinating%20who%20works%20on%20what%22
