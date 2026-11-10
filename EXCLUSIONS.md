# Local Artifact Exclusions & Reconstruction Guide (`EXCLUSIONS.md`)

This file explicitly documents all files, heavy dependencies, and temporary artifacts that are excluded from this Git repository via `.gitignore`, why they were excluded, and how to rebuild or regenerate them locally on your machine.

---

## 1. Excluded Items Summary Table

| Excluded Item / Pattern | Category | Size / Scope | Reason for Git Exclusion | Local Reconstruction Method |
| :--- | :--- | :--- | :--- | :--- |
| `zsign_src/build/windows/vs2022/lib/openssl/` (`libcrypto.lib`, `libssl.lib`) | **Heavy Third-Party Dependency** | ~66.5 MB binary | Static C/C++ libraries exceed optimal Git tracking size; can be rebuilt or downloaded from official OpenSSL sources. | See [Section 2.1](#21-reconstructing-openssl-static-libraries) below. |
| `bin/zsign.exe`, `zsign_src/bin/zsign.exe`, `zsign_src/.../x64/Release/zsign.exe` | **Precompiled Binary** | ~4.4 MB binary | Binaries should not be tracked when full source code is versioned. | See [Section 2.2](#22-rebuilding-zsignexe-from-source) below. |
| `zsign_src/build/windows/vs2022/zsign/x64/Release/` (`*.obj`, `*.iobj`, `*.ipdb`, `*.pdb`, `*.tlog`) | **Intermediate Build Outputs** | ~25 MB | Compiler temporary files and symbol databases generated during Visual Studio compilation. | Automatically regenerated when compiling the VS2022 solution. |
| `x64/`, `build/` | **Build Directories** | Variable | Local test output logs and MSBuild intermediate output. | Regenerated during CMake or MSBuild runs. |
| `air_capture*.etl`, `air_capture*.pcapng`, `air_capture*.txt` | **Hardware Packet Captures** | ~175 MB total (`air_capture2_brief.txt` is 122 MB) | High-volume raw network capture dumps from the RoboSense Airy test runs. | See [Section 2.3](#23-re-generating-hardware-packet-captures) below. |
| `netstat.txt` | **Diagnostics Dump** | <1 KB | Ephemeral network status snapshot. | Regenerated via `netstat -an`. |
| `.vs/`, `DerivedData/`, `xcuserdata/` | **IDE User Preferences** | Variable | User-specific IDE states and workspace caches. | Generated automatically by Visual Studio or Xcode. |

---

## 2. Local Reconstruction Instructions

### 2.1 Reconstructing OpenSSL Static Libraries
The `zsign` IPA code-signing tool requires OpenSSL 1.1.1 or 3.x static libraries for Windows x64 with the static Multi-Threaded runtime (`/MT`).

- **Target Destination**:
  `zsign_src/build/windows/vs2022/lib/openssl/x64/mt/`
  - `libcrypto.lib`
  - `libssl.lib`

**Option A: Automated Download via vcpkg**
```powershell
git clone https://github.com/microsoft/vcpkg.git C:\tools\vcpkg
C:\tools\vcpkg\bootstrap-vcpkg.bat
C:\tools\vcpkg\vcpkg.exe install openssl:x64-windows-static

# Copy libraries to zsign tree
mkdir zsign_src\build\windows\vs2022\lib\openssl\x64\mt -Force
copy C:\tools\vcpkg\installed\x64-windows-static\lib\libcrypto.lib zsign_src\build\windows\vs2022\lib\openssl\x64\mt\
copy C:\tools\vcpkg\installed\x64-windows-static\lib\libssl.lib zsign_src\build\windows\vs2022\lib\openssl\x64\mt\
```

**Option B: Using Pre-compiled Windows OpenSSL Distribution**
Download the Win64 OpenSSL v3.x binary installer (with static libraries), or extract the `/MT` compiled `.lib` files into the directory specified above.

---

### 2.2 Rebuilding `zsign.exe` from Source
The complete source code for `zsign` is fully tracked inside [`zsign_src/`](./zsign_src/).

To compile `zsign.exe` on Windows:

1. Open **Developer PowerShell for VS 2022** (or standard PowerShell with MSBuild in `PATH`).
2. Run:
```powershell
msbuild zsign_src\build\windows\vs2022\zsign.sln /p:Configuration=Release /p:Platform=x64
```
3. Copy the compiled executable to the project `bin/` directory:
```powershell
mkdir bin -Force
copy zsign_src\build\windows\vs2022\x64\Release\zsign.exe bin\zsign.exe
```

---

### 2.3 Re-generating Hardware Packet Captures
The raw test files (`air_capture.etl`, `air_capture2.etl`, `air_capture.pcapng`, `air_capture2_brief.txt`) were generated during the physical RoboSense Airy bringup test.

To capture fresh packets from the sensor at any time:

1. Ensure the RoboSense Airy is attached via the USB-C Ethernet adapter.
2. In an **Administrator PowerShell**, find the adapter component ID:
```powershell
pktmon list
```
3. Start full packet capture (replace `<ID>` with the Realtek adapter ID, e.g. `119`):
```powershell
pktmon start -c --pkt-size 0 --comp <ID> --file-name air_capture.etl
```
4. Power-cycle the sensor or run operations for 30 seconds.
5. Stop the capture:
```powershell
pktmon stop
```
6. Convert to standard Wireshark PCAPNG format or readable text:
```powershell
pktmon etl2pcap air_capture.etl --out air_capture.pcapng
pktmon etl2txt air_capture.etl -b --out air_capture_brief.txt
```

---

### 2.4 Generating the CMake Build Directory
To generate the C++17 build tree:
```powershell
mkdir build -Force
cd build
cmake ..
cmake --build . --config Release
```
