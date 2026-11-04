<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# 3. Preparar un Linux nativo con la GPU de verdad

En WSL, Vulkan corre en `llvmpipe` (por software), así que no sirve para juzgar la ventana: va lento
y no es tu GPU. Para eso hace falta un Linux que arranque directamente en el PC.

## Partición, no máquina virtual

| Opción | ¿Ve tu GPU? | Comentario |
|---|---|---|
| **Partición (dual boot)** | Sí | **La recomendada.** Unos 80 GB bastan (el juego compilado ocupa unos 10 GB, más las etapas y las herramientas). |
| Linux en un SSD externo por USB | Sí | Igual de bueno, sin tocar el disco de Windows. Un USB 3 rápido; un pendrive lento hace la compilación eterna. |
| Máquina virtual (VirtualBox, VMware, Hyper-V) | **No** | Te da una GPU virtual o `llvmpipe`, el mismo problema que WSL. Pasar la GPU de verdad a una VM (passthrough) exige una segunda GPU y configurar IOMMU: no vale la pena. |

Antes de partir el disco:
- Hacer una copia de lo importante.
- En Windows, desactivar el **inicio rápido** (Panel de control › Opciones de energía) y, si usas
  BitLocker, tener a mano la clave de recuperación.
- Achicar la partición de Windows desde **Administración de discos**, no desde el instalador de
  Linux.

## La distribución

**Ubuntu 24.04 LTS** (o Kubuntu/Xubuntu 24.04): es la de la CI y la de los comandos del README.
Si tu WSL también es 24.04, las cosas que copies de ahí se comportan igual.

Después de instalar:

1. Los drivers de la GPU:
   - **NVIDIA**: `sudo ubuntu-drivers install` y reiniciar (el driver propietario; el libre,
     `nouveau`, no sirve para Vulkan).
   - **AMD o Intel**: ya vienen (Mesa). Basta con tener el sistema al día
     (`sudo apt update && sudo apt full-upgrade`).
2. Comprobar que Vulkan ve tu GPU, y no `llvmpipe`:
   ```bash
   sudo apt install vulkan-tools
   vulkaninfo --summary | grep -E 'deviceName|driverName'
   ```
   Tiene que salir el nombre de tu GPU. Si solo sale `llvmpipe`, falta el driver.
3. Los paquetes para compilar (los del README, más git y los tests de Vulkan):
   ```bash
   sudo apt install git cmake ninja-build clang python3 make pkg-config ca-certificates libvulkan1 \
     libasound2-dev libpulse-dev libpipewire-0.3-dev libx11-dev libxext-dev libxrandr-dev \
     libxcursor-dev libxi-dev libxss-dev libxkbcommon-dev libwayland-dev wayland-protocols \
     libegl-dev libdecor-0-dev libudev-dev libdbus-1-dev vulkan-validationlayers
   ```

## Traer el port y el juego

Hay dos caminos. El primero es el del jugador y no depende de nada de WSL; el segundo ahorra
horas si quieres reusar las etapas.

### Camino A: como un jugador (recomendado para empezar)

```bash
git clone --recursive -b claude/modest-ptolemy-y9sau6 \
  https://github.com/Alvaro8317/mgs-portable-ops-pc-port.git ~/mpo
cd ~/mpo
python3 -I profiles/mpo/scripts/setup.py install --iso ~/Juegos/ULUS-10202.iso --fonts ~/Juegos/flash0/font
python3 -I profiles/mpo/scripts/setup.py stages --low-priority     # en otra terminal; horas
python3 -I profiles/mpo/scripts/setup.py play
```

- La instalación tarda de 20 a 40 minutos. Después ya se juega: lo que no está compilado corre en
  el intérprete, más lento.
- Las 92 etapas tardan horas (6 h 45 min a `-j5` en F21). Se puede jugar mientras tanto, y cada
  etapa compilada se usa en cuanto está.
- El ISO y las fuentes se copian de Windows (ver abajo).

### Camino B: reusar las etapas que ya compilaste en WSL

Las bibliotecas de las etapas de WSL (`build-f09-release/overlays/*.so`) son de Linux x86-64 y
deberían cargar en un Linux nativo con la misma Ubuntu y el mismo Clang. Para comprobarlo con una:

```bash
<build del juego>/profiles/mpo/host/hle/mpo_boot --check-overlay ~/mpo-wsl/overlays/<una>.so
```

Si sale bien, se pueden usar con `--overlays <carpeta>` en las pruebas de
[04-linux-nativo-ventana.md](04-linux-nativo-ventana.md), igual que en WSL. Si falla, el Camino A.

## Copiar cosas de Windows o de WSL

Desde el Linux nativo se ve la partición de Windows (en el gestor de archivos, o en `/media/...`).
La forma más fácil:

1. En WSL, copiar a una carpeta de Windows lo que vas a necesitar, por ejemplo:
   ```bash
   mkdir -p /mnt/c/mpo-linux
   cp -a ~/<tu checkout>/analysis /mnt/c/mpo-linux/       # grabaciones y scripts (si vas a repetir)
   cp -a ~/<tu checkout>/build-f09-release/overlays /mnt/c/mpo-linux/   # solo para el Camino B
   ```
   El ISO y la carpeta `flash0/font` normalmente ya están en Windows.
2. Arrancar Linux y copiar `C:\mpo-linux` (y el ISO y las fuentes) a tu carpeta personal.
   **Copiar**, no trabajar sobre la partición de Windows: NTFS desde Linux es lento y no guarda
   bien los permisos.

Nada de esto (ISO, `analysis/`, etapas) va al repo; las carpetas ya están en `.gitignore`.

## Listo cuando

- [ ] `vulkaninfo --summary` muestra tu GPU.
- [ ] `setup.py status` dice que el juego está instalado.
- [ ] `setup.py play` abre la ventana y el juego arranca.

Después, [04-linux-nativo-ventana.md](04-linux-nativo-ventana.md).
