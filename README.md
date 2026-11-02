# Singularity

Singularity is the shared workplace of a law firm or a legal department, in which people and AI colleagues work in one
structure and carry legal work from the first discussion to an accepted result.

An AI colleague in Singularity is a member of the firm, with a role, an area and a record, beside the people. It takes
work in a Matter's threads, finds the documents it needs, delivers its result with the evidence for it, and comes back
with what it found. A person gives it a goal and bounds, not steps.

- **Matters.** Each Matter holds its topic threads, its documents and its work products. Members, people and AI
  colleagues alike, discuss and decide in its threads.
- **The firm's own computers.** Singularity runs on macOS, Windows and Linux. A Matter's records live beside its
  documents, in the Matter's own folder. The firm's computers share a Matter directly, sealed end to end, and no file
  of the firm passes through a server of Legal Machines.
- **Papinian Guard.** Before any request reaches an external model, Papinian Guard takes the client's names, figures
  and other protected facts out of it on the lawyer's computer, and puts them back into the answer there. When
  protection cannot run, nothing is sent.
- **Models.** The firm connects its own model accounts (OpenAI, Anthropic, Google, OpenRouter and others), and runs
  local models on its own computers through Singularity's local inference, built on llama.cpp.
- **Office.** Add-ins for Word, Excel, PowerPoint and Outlook work with the same Matters.

## Repository

| Folder | What it holds |
| --- | --- |
| `code/backend` | The native core in C++17, by domain (`Matter`, `Chat`, `Document`, `Agent`, `PapinianGuard`, ...), and the Native Bridge to the window |
| `code/frontend` | The window: React, in JavaScript and JSX |
| `docs/contracts` | The Native API: every call between the window and the core |
| `deploy` | The build: CMake for the core, npm and Vite for the window, build containers and scripts |
| `dependencies/llama.cpp` | The pinned llama.cpp source local inference is built from, and the patches applied to it |
| `legal` | The documents that govern Legal Machines' account service |

Each component of the core keeps its header and its implementation side by side.

## Building

Every platform needs CMake 3.24 or later, Node.js with npm, Python 3.9 or later, a Rust toolchain (the Papinian NER
runtime is built from Rust) and GNU `patch`. The build downloads the pinned llama.cpp source that local inference runs,
checks it against its SHA-256 and applies the patches in `dependencies/llama.cpp`.

- **macOS:** Xcode.
- **Windows:** Visual Studio 2022 with Desktop C++, vcpkg with `zlib:x64-windows-static` (found through
  `VCPKG_INSTALLATION_ROOT`, `VCPKG_ROOT`, `PATH` or the copy Visual Studio installs) and the WebView2 Runtime; NSIS
  to make the installer. With the CUDA toolkit, local inference runs on NVIDIA GPUs; without it, on the CPU.
- **Linux (Debian or Ubuntu):**

  ```sh
  sudo apt install build-essential cmake ninja-build pkg-config \
    libgtk-3-dev libwebkit2gtk-4.1-dev libwayland-dev wayland-protocols dpkg-dev \
    libsecret-1-dev dbus-daemon gnome-keyring
  ```

  `deploy/containers/linux-x64.Dockerfile` is the image the Linux release is built in.

Then build the Debug app and open it against a live development server of the window:

```sh
npm --prefix deploy/frontend install
python3 deploy/scripts/dev.py
```

For a release build and its installer, run the platform's script on that platform:

```sh
deploy/scripts/build-macos.sh       # macOS: the app and its disk image
deploy/scripts/build-windows.ps1    # Windows: the installer, with MSVC and NSIS
deploy/scripts/build-linux.sh       # Linux: the .deb, built in the Linux image
```

The packages are written under `deploy/backend/out`.

## License

Singularity is source-available under the [Legal Machines Singularity License](LICENSE.md): its source code is
public, and it may be used only within your organization and only for legal work, and may not be distributed; see
[NOTICE](NOTICE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The model behind Papinian Guard and Legal
Machines' account service are not part of this repository: THIRD_PARTY_NOTICES.md names the model and its licenses, and
the documents in [legal](legal) govern the account service.

Report a vulnerability privately to inbox@legalmachines.org, with "Security" in the subject, never in a public issue.
