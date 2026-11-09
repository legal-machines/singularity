#!/usr/bin/env python3
"""Single developer entry point for Singularity builds, packages and local reset."""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FRONTEND = ROOT / "deploy" / "frontend"
BACKEND = ROOT / "deploy" / "backend"
METADATA = BACKEND / "cmake" / "AppMetadata.cmake"
DEV_PORT = 5173
HOST_PLATFORMS = {"Darwin": "macos", "Windows": "windows", "Linux": "linux"}
CONTAINER_ROOT = ROOT / "deploy" / "containers"
CONTAINER_IMAGES = {
    "linux": "singularity-linux-x64-builder:0.0.2",
    "windows": "singularity-windows-x64-builder:0.0.2",
}
# The hosts that build the Linux and Windows packages in those containers.
# Docker runs the linux/amd64 images natively on a Linux host - WSL on a
# Windows computer among them - and emulated on Apple silicon.
CONTAINER_HOSTS = ("macos", "linux")
# A release builds local inference's llama.cpp runtime for every computer it
# may run on, not for the one building it (deploy/backend/cmake/LocalInferenceRuntime.cmake).
PORTABLE_RUNTIME = "-DLOCAL_INFERENCE_PORTABLE=ON"
# The Linux container's HOME: the invoking user has none in the image.
CONTAINER_HOME = "/tmp/singularity-home"
# The Linux tests' own homes, on the container's disk rather than the shared
# source folder: a Mac shares that folder through virtiofs, which refuses
# libstdc++'s std::filesystem::copy_file and carries no Unix socket
# (SINGULARITY_TEST_HOMES_ROOT in deploy/backend/CMakeLists.txt).
CONTAINER_TEST_HOMES = f"{CONTAINER_HOME}/test-homes"


def configure_utf8_stdio() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if callable(reconfigure):
            reconfigure(encoding="utf-8")


def require_command(command: str) -> None:
    if shutil.which(command) is None:
        raise RuntimeError(f"A required command was not found: {command}")


def cmake_value(name: str) -> str:
    content = METADATA.read_text(encoding="utf-8")
    match = re.search(rf'^set\({re.escape(name)} "([^"]+)"\)$', content, re.MULTILINE)
    if not match:
        raise RuntimeError(f"No value for {name} in {METADATA}")
    return match.group(1)


def host_platform() -> str:
    try:
        return HOST_PLATFORMS[platform.system()]
    except KeyError as error:
        raise RuntimeError(f"Unsupported operating system: {platform.system()}") from error


def resolve_platform(requested: str) -> str:
    current = host_platform()
    return current if requested == "auto" else requested


def preset_name(target: str, mode: str) -> str:
    return target if mode == "release" else f"{target}-dev"


def run_checked(command: list[str], cwd: Path) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def build_container_image(target: str) -> str:
    image = CONTAINER_IMAGES[target]
    dockerfile = CONTAINER_ROOT / f"{target}-x64.Dockerfile"
    run_checked([
        "docker", "build", "--platform", "linux/amd64", "--file", str(dockerfile),
        "--tag", image, str(CONTAINER_ROOT),
    ], ROOT)
    return image


def uses_container(command: str, target: str, host: str) -> bool:
    """Whether `command` builds `target`'s package in its container on `host`.

    A Linux release is built in its container, from a Linux host too,
    so every Linux package comes from the same image. The Windows release is
    built natively on a Windows PC with MSVC and the CUDA toolkit
    (deploy/scripts/build-windows.ps1), so local inference keeps the
    GPU; from another OS its container builds only the CPU-only cross check.
    """
    if command != "release" or target not in CONTAINER_IMAGES:
        return False
    return target != host or target == "linux"


def build_container_package(target: str, *, clean_build: bool) -> None:
    if host_platform() not in CONTAINER_HOSTS:
        raise RuntimeError(
            "The Windows and Linux container builds run on a macOS or Linux host; on Windows, run them from WSL"
        )
    require_command("docker")
    if clean_build:
        build_name = "windows-cross" if target == "windows" else "linux"
        remove_tree(BACKEND / "build" / build_name, expected_names={build_name})
        remove_tree(BACKEND / "out" / target, expected_names={target})
    prepare_container_folders(target)
    image = build_container_image(target)
    passwd = None
    if target == "linux":
        image_passwd = subprocess.run(
            ["docker", "run", "--rm", "--platform", "linux/amd64", image, "cat", "/etc/passwd"],
            check=True, capture_output=True, text=True,
        ).stdout
        passwd = BACKEND / "build" / "linux-container" / "passwd"
        passwd.parent.mkdir(parents=True, exist_ok=True)
        passwd.write_text(container_passwd(image_passwd, os.getuid(), os.getgid()), encoding="utf-8")
    run_checked(container_run_command(target, image, container_build_commands(target), passwd), ROOT)
    if target == "linux":
        run_checked(linux_package_check_command(built_linux_package()), ROOT)


# The clean system the Linux package is installed on before it is called made
# (deploy/containers/check-linux-package.sh): the builder's own base, with
# nothing of the build in it.
LINUX_CHECK_IMAGE = "ubuntu:24.04"


def container_output_folders(target: str, backend: Path = BACKEND) -> list[Path]:
    """The folders a container build writes beside the source: deploy/backend's
    build and out, the build tree and package folder of `target`, and for Linux
    the folder of the passwd file the container reads."""
    build_name = "windows-cross" if target == "windows" else "linux"
    folders = [backend / "build", backend / "out", backend / "build" / build_name, backend / "out" / target]
    if target == "linux":
        folders.append(backend / "build" / "linux-container")
    return folders


def prepare_container_folders(target: str, backend: Path = BACKEND) -> None:
    """Makes deploy/backend's build and out as the invoking user before any
    container runs. The Windows container runs as root. On a Linux host the
    folders it made itself would belong to root, and the Linux container,
    which runs as the invoking user, could not write them. A Linux build stops
    before its container when an earlier one left them root's, and says what
    to do."""
    for folder in (backend / "build", backend / "out"):
        folder.mkdir(parents=True, exist_ok=True)
    if target != "linux":
        return
    blocked = [folder for folder in container_output_folders(target, backend)
               if folder.exists() and not os.access(folder, os.W_OK)]
    if blocked:
        raise RuntimeError(
            "The build folders belong to another user after an earlier container build ("
            + ", ".join(str(folder) for folder in blocked)
            + "); take them back with: sudo chown -R $(id -u):$(id -g) deploy/backend/build deploy/backend/out"
        )


def built_linux_package() -> Path:
    packages = sorted((BACKEND / "out" / "linux").glob("*.deb"), key=lambda path: path.stat().st_mtime)
    if not packages:
        raise RuntimeError(f"No Linux package found in {BACKEND / 'out' / 'linux'}")
    return packages[-1]


def linux_package_check_command(package: Path) -> list[str]:
    """Installs `package` into a clean Ubuntu and checks that every program
    in it finds what it needs (check-linux-package.sh)."""
    return [
        "docker", "run", "--rm", "--platform", "linux/amd64",
        "--volume", f"{package.parent}:/package:ro",
        "--volume", f"{CONTAINER_ROOT}:/check:ro",
        LINUX_CHECK_IMAGE, "bash", "/check/check-linux-package.sh", f"/package/{package.name}",
    ]


def container_passwd(image_passwd: str, uid: int, gid: int) -> str:
    """The image's /etc/passwd with the invoking user in it. The Linux build
    runs as that user (container_user_arguments), whom the image may not know,
    and D-Bus closes a connection from a user with no name, so the key store's
    test (beta1_credential_store), which runs a private session bus, would
    fail. A user the image already has (uid 1000 is Ubuntu's own) stays as it
    is."""
    lines = [line for line in image_passwd.splitlines() if line.strip()]
    if not any(line.split(":")[2:3] == [str(uid)] for line in lines):
        lines.append(f"singularity:x:{uid}:{gid}:Singularity build:{CONTAINER_HOME}:/bin/bash")
    return "\n".join(lines) + "\n"


def container_build_commands(target: str) -> list[str]:
    """What runs inside `target`'s container: configure, build, the Linux
    tests, and the package. Windows' cross-build runs no test (MinGW's
    programs do not run there)."""
    preset = "windows-cross" if target == "windows" else "linux"
    configure = f"cmake --preset {preset} {PORTABLE_RUNTIME}"
    if target == "linux":
        configure += f" -DSINGULARITY_TEST_HOMES_ROOT={CONTAINER_TEST_HOMES}"
    commands = [configure, f"cmake --build --preset {preset}"]
    if target == "linux":
        commands.insert(0, 'mkdir -p "$HOME"')
        commands.append("ctest --preset linux")
    commands.append(f"cpack --preset {preset}")
    return commands


def container_run_command(target: str, image: str, commands: list[str], passwd: Path | None = None) -> list[str]:
    passwd_volume = ["--volume", f"{passwd}:/etc/passwd:ro"] if passwd is not None else []
    return [
        "docker", "run", "--rm", "--platform", "linux/amd64",
        *container_user_arguments(target),
        *passwd_volume,
        "--volume", f"{ROOT}:/workspace", "--workdir", "/workspace/deploy/backend",
        image, "bash", "-lc", " && ".join(commands),
    ]


def container_user_arguments(target: str) -> list[str]:
    # The Linux build, its ctest and CPack run as the invoking user, never as
    # root: root ignores file permissions, so the tests that prove an
    # unreadable or read-only file is refused (matter_registry,
    # document_version_store) fail under it. That user has no home in the
    # image, so HOME and cargo's downloads go to a folder of the container's
    # own; the Rust toolchain itself is readable by everyone (RUSTUP_HOME in
    # deploy/containers/linux-x64.Dockerfile).
    if target != "linux":
        return []
    return [
        "--user", f"{os.getuid()}:{os.getgid()}",
        "--env", f"HOME={CONTAINER_HOME}",
        "--env", f"CARGO_HOME={CONTAINER_HOME}/.cargo",
    ]


def remove_tree(path: Path, *, expected_names: set[str]) -> None:
    resolved = path.expanduser().resolve(strict=False)
    if resolved.name not in expected_names:
        raise RuntimeError(f"Refusing to clean an unexpected path: {resolved}")
    if not resolved.exists() and not resolved.is_symlink():
        print(f"Skipped (no data): {resolved}")
        return
    if resolved.is_symlink() or resolved.is_file():
        resolved.unlink()
    else:
        shutil.rmtree(resolved)
    print(f"Removed: {resolved}")


def app_data_paths(target: str) -> list[Path]:
    slug = cmake_value("NATIVE_APP_SLUG")
    identifier = cmake_value("NATIVE_APP_IDENTIFIER")
    home = Path.home()
    if target == "macos":
        return [
            home / "Library" / "Application Support" / slug,
            home / "Library" / "Caches" / slug,
            home / "Library" / "Caches" / identifier,
            home / "Library" / "WebKit" / identifier,
            home / "Library" / "Saved Application State" / f"{identifier}.savedState",
        ]
    if target == "windows":
        appdata = os.environ.get("APPDATA")
        local_appdata = os.environ.get("LOCALAPPDATA")
        if not appdata:
            raise RuntimeError("APPDATA is not set, so the application's data cannot be found safely")
        # The preferences roam; this computer's own data stays local.
        data_name = cmake_value("NATIVE_APP_DATA_NAME")
        paths = [Path(appdata) / slug]
        if local_appdata:
            local = Path(local_appdata)
            paths.extend([local / slug, local / data_name])
        return paths
    config = Path(os.environ.get("XDG_CONFIG_HOME", home / ".config"))
    cache = Path(os.environ.get("XDG_CACHE_HOME", home / ".cache"))
    data = Path(os.environ.get("XDG_DATA_HOME", home / ".local" / "share"))
    return [config / slug, cache / slug, data / slug]


def clean_app_data(target: str) -> None:
    print("Cleaning only Singularity's own app data; Matter folders and .singularity-workspace stay.")
    allowed = {
        cmake_value("NATIVE_APP_SLUG"),
        cmake_value("NATIVE_APP_IDENTIFIER"),
        f"{cmake_value('NATIVE_APP_IDENTIFIER')}.savedState",
        cmake_value("NATIVE_APP_DATA_NAME"),
    }
    for path in app_data_paths(target):
        remove_tree(path, expected_names=allowed)
    if target == "linux" and host_platform() == "linux":
        clear_linux_keyring_credentials()


# Provider/CredentialStore/CredentialStoreLinux.cpp keeps provider keys in the
# desktop keyring, not in the settings folder, so they are cleared there.
LINUX_CREDENTIAL_SCHEMA = "ai.legalmachines.singularity.Credential"


def clear_linux_keyring_credentials() -> None:
    slug = cmake_value("NATIVE_APP_SLUG")
    if shutil.which("secret-tool") is None:
        print("Model keys in the keyring were not removed: the secret-tool command is needed (package libsecret-tools).")
        return
    subprocess.run(
        ["secret-tool", "clear", "xdg:schema", LINUX_CREDENTIAL_SCHEMA, "application", slug],
        check=False,
    )
    print(f"Model keys of {slug} were removed from the keyring.")


def clean_build_outputs(target: str, mode: str) -> None:
    preset = preset_name(target, mode)
    remove_tree(BACKEND / "build" / preset, expected_names={preset})
    if mode == "release":
        remove_tree(BACKEND / "out" / target, expected_names={target})
    remove_tree(FRONTEND / "dist", expected_names={"dist"})


def npm_command() -> str:
    return "npm.cmd" if os.name == "nt" else "npm"


def prepare_frontend(mode: str) -> None:
    npm = npm_command()
    if mode == "release":
        run_checked([npm, "ci"], FRONTEND)
        run_checked([npm, "run", "check"], FRONTEND)
    else:
        if not (FRONTEND / "node_modules").is_dir():
            run_checked([npm, "install"], FRONTEND)
        run_checked([npm, "run", "build"], FRONTEND)
    run_checked([sys.executable, str(ROOT / "deploy" / "scripts" / "verify_contract.py")], ROOT)


def native_executable(target: str, mode: str) -> Path:
    display_name = cmake_value("NATIVE_APP_DISPLAY_NAME")
    slug = cmake_value("NATIVE_APP_SLUG")
    configuration = "Release" if mode == "release" else "Debug"
    preset = preset_name(target, mode)
    if target == "macos":
        return BACKEND / "build" / preset / configuration / f"{display_name}.app" / "Contents" / "MacOS" / display_name
    if target == "windows":
        return BACKEND / "build" / preset / configuration / f"{display_name}.exe"
    return BACKEND / "build" / preset / slug


def build_native(target: str, mode: str) -> Path:
    preset = preset_name(target, mode)
    configure = ["cmake", "--preset", preset]
    if mode == "release":
        configure.append(PORTABLE_RUNTIME)
    run_checked(configure, BACKEND)
    build_command = ["cmake", "--build", "--preset", preset]
    if mode != "release":
        # The `dev` product-owner Debug handoff proves the real application
        # builds and launches; it must not require every unrelated test
        # executable in the repository to compile (those are task-specific
        # acceptance gates, run separately).
        # `native_app` is the same real application target name on macOS,
        # Windows and Linux (deploy/backend/CMakeLists.txt). Release keeps
        # building everything, since it runs the full ctest suite next.
        build_command += ["--target", "native_app"]
    run_checked(build_command, BACKEND)
    executable = native_executable(target, mode)
    if not executable.is_file():
        raise RuntimeError(f"Native executable not found: {executable}")
    if target == "macos":
        refresh_macos_bundle_icon(executable.parents[2])
    return executable


LSREGISTER = Path(
    "/System/Library/Frameworks/CoreServices.framework/Frameworks/"
    "LaunchServices.framework/Support/lsregister"
)


def refresh_macos_bundle_icon(bundle: Path) -> None:
    # Xcode replaces app.icns inside the bundle but leaves the bundle's own
    # date, so the Dock and Finder kept the icon they had cached for it: a
    # Debug build showed the old icon instead of the Kitsune.
    os.utime(bundle)
    if LSREGISTER.is_file():
        subprocess.run([str(LSREGISTER), "-f", str(bundle)], check=False)


def is_port_taken(port: int) -> bool:
    """Whether something on this computer already answers on, or holds, the port."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            return True
    except OSError:
        pass
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.bind(("127.0.0.1", port))
        return False
    except OSError:
        return True
    finally:
        probe.close()


def free_dev_port(first: int = DEV_PORT, tries: int = 20) -> int:
    """The first port from `first` that nothing holds. Another project's dev server
    on Singularity's port once had the Debug window open on its own page
    (2026-11-08): Singularity's server never shares a port, it takes the next."""
    for port in range(first, first + tries):
        if not is_port_taken(port):
            return port
    raise RuntimeError(f"Ports {first} to {first + tries - 1} are all in use on this computer: close another dev server, "
                       "then run dev.py again")


def wait_for_vite(process: subprocess.Popen[bytes], port: int, timeout: float = 20.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Vite ended before it opened port {port}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                pass
        except OSError:
            time.sleep(0.1)
            continue
        # What answers is ours only while our Vite still runs: one that could not
        # have the port ends at once.
        time.sleep(0.5)
        if process.poll() is not None:
            raise RuntimeError(f"Vite ended after port {port} answered: another program holds it")
        return
    raise RuntimeError(f"Vite did not open port {port} within {int(timeout)} seconds")


def stop_dev_server(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        # npm.cmd runs Vite as a grandchild (cmd.exe -> node.exe): ending
        # cmd.exe alone would leave Vite holding its port for the next run.
        subprocess.run(
            ["taskkill", "/T", "/F", "/PID", str(process.pid)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        process.wait()
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def launch_development(executable: Path, onboarding: str | None = None) -> int:
    # Deliberately detach both the dev server and the real app into their own
    # POSIX session (setsid), never the calling shell's process group/session.
    # `dev.py` itself is normally invoked from an interactive terminal, but the
    # same command is also the canonical product-owner Debug handoff run from
    # inside an automated implementation session - a foreground command whose
    # own process group can be reaped by that session's own tooling once the
    # command returns control. Without this, that reap silently takes the real
    # Singularity window and Vite down with it, even though neither actually
    # failed - the exact "clean exit code 0, no crash report, app just gone
    # after a short unattended period" pattern this fixes. `start_new_session`
    # is POSIX-only; the equivalent problem does not arise from Windows' own
    # different job-object/console-group model, so it is applied only there.
    popen_kwargs = {"start_new_session": True} if os.name == "posix" else {}
    port = free_dev_port()
    if port != DEV_PORT:
        print(f"Port {DEV_PORT} is in use by another program; Singularity's frontend runs on {port}")
    vite = subprocess.Popen([npm_command(), "run", "dev", "--", "--port", str(port)], cwd=FRONTEND, **popen_kwargs)
    try:
        wait_for_vite(vite, port)
        environment = os.environ.copy()
        environment["NATIVE_APP_DEV_SERVER_URL"] = f"http://127.0.0.1:{port}"
        if onboarding is not None:
            # "1" begins onboarding afresh, "0" marks it complete: a
            # development build records that, and the window and the page
            # open on onboarding or on the app.
            environment["NATIVE_APP_ONBOARDING"] = onboarding
        return subprocess.run([str(executable)], cwd=ROOT, env=environment, **popen_kwargs).returncode
    except KeyboardInterrupt:
        return 130
    finally:
        stop_dev_server(vite)


def execute_build(args: argparse.Namespace) -> int:
    target = resolve_platform(args.platform)
    if target == "all":
        if args.command != "release" or host_platform() != "macos":
            raise RuntimeError("--platform all is available only for release on macOS")
        require_command("cmake")
        require_command("npm")
        if args.clean_data:
            clean_app_data("macos")
        if args.clean_build:
            clean_build_outputs("macos", "release")
        prepare_frontend("release")
        executable = build_native("macos", "release")
        run_checked(["ctest", "--preset", "macos"], BACKEND)
        run_checked(["cpack", "--preset", "macos"], BACKEND)
        build_container_package("linux", clean_build=args.clean_build)
        build_container_package("windows", clean_build=args.clean_build)
        print(f"macOS app built: {executable}")
        print("Windows is built here only as a CPU check (windows-cross); the Windows installer "
              "is built natively on a Windows PC with MSVC and CUDA.")
        print(f"All local packages: {BACKEND / 'out'}")
        return 0
    if uses_container(args.command, target, host_platform()):
        require_command("npm")
        prepare_frontend("release")
        build_container_package(target, clean_build=args.clean_build)
        print(f"Installer or package: {BACKEND / 'out' / target}")
        return 0
    if target != host_platform():
        raise RuntimeError(f"Cannot build {target} on a {host_platform()} host")
    require_command("cmake")
    require_command("npm")
    if args.clean_data:
        clean_app_data(target)
    if args.clean_build:
        clean_build_outputs(target, args.command)
    prepare_frontend(args.command)
    executable = build_native(target, args.command)
    if args.command == "release":
        preset = preset_name(target, "release")
        run_checked(["ctest", "--preset", preset], BACKEND)
        run_checked(["cpack", "--preset", preset], BACKEND)
        print(f"App built: {executable}")
        print(f"Installer or package: {BACKEND / 'out' / target}")
        return 0
    print(f"Development build ready: {executable}")
    return 0 if args.no_run else launch_development(executable, onboarding=args.onboarding)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    subcommands = result.add_subparsers(dest="command", required=True)
    for name in ("dev", "release"):
        command = subcommands.add_parser(name)
        platforms = ("auto", "macos", "windows", "linux", "all") if name == "release" else ("auto", "macos", "windows", "linux")
        command.add_argument("--platform", choices=platforms, default="auto")
        command.add_argument("--clean-data", action="store_true", help="Remove Singularity's own settings, credentials and caches")
        command.add_argument("--clean-build", action="store_true", help="Remove old build and package outputs")
        if name == "dev":
            command.add_argument("--no-run", action="store_true", help="Build only; start neither the app nor Vite")
            start = command.add_mutually_exclusive_group()
            start.add_argument("--onboarding", dest="onboarding", action="store_const", const="1", help="Start onboarding again: the 1080×720 window and sign-in")
            start.add_argument("--skip-onboarding", dest="onboarding", action="store_const", const="0", help="Skip onboarding: open the app directly")
    clean = subcommands.add_parser("clean-data")
    clean.add_argument("--platform", choices=("auto", "macos", "windows", "linux"), default="auto")
    return result


def main() -> int:
    args = parser().parse_args()
    if args.command == "clean-data":
        target = resolve_platform(args.platform)
        if target != host_platform():
            raise RuntimeError("clean-data runs only for this host's operating system")
        clean_app_data(target)
        return 0
    return execute_build(args)


def cli() -> None:
    configure_utf8_stdio()
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nBuild cancelled.", file=sys.stderr)
        raise SystemExit(130)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    cli()
