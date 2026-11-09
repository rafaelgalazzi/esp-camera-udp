"""Build a complete flash image for web flashers, written at offset 0x0."""
from pathlib import Path
import subprocess

Import("env")

images = [(str(offset), env.subst(str(path))) for offset, path in env.get("FLASH_EXTRA_IMAGES", [])]
images.append((env.subst("$ESP32_APP_OFFSET"), env.subst("$BUILD_DIR/${PROGNAME}.bin")))


def merge_images(source, target, env):
    destination = Path(str(target[0]))
    temporary = destination.with_suffix(".bin.tmp")
    tool = Path(env.PioPlatform().get_package_dir("tool-esptoolpy")) / "esptool.py"
    command = [env.subst("$PYTHONEXE"), str(tool), "--chip", env.BoardConfig().get("build.mcu"),
               "merge_bin", "-o", str(temporary)]
    for (offset, _), node in zip(images, source):
        command.extend([offset, str(node)])
    subprocess.run(command, check=True)
    temporary.replace(destination)
    print(f"Complete firmware: {destination} (flash at 0x0)")


merged = env.Command("$BUILD_DIR/firmware-merged.bin", [path for _, path in images], merge_images)
env.Default(merged)
env.Alias("upload", merged)
