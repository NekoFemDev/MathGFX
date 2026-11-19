# MathGFX

Software procedural renderer: собственный ASM-like DSL → байткод → VM →
RGB8-кадр → PNG. Основной путь — CPU; тяжёлые сцены можно выносить в
headless Vulkan compute, по одному шейдеру на эффект. Видео пишется
встроенным H.264/MP4-путём (libx264 + свой MP4-муксер) или MJPEG/AVI
fallback — без ffmpeg. Никаких диффузионных моделей — только математика.

`Project.md` — исходная концепция. Этот README описывает то, что уже
реализовано в v0.1.

---

## Быстрый старт

```sh
make
./build/mathgfx render examples/gradient.mgf out/gradient.png
```

Шесть сцен сразу:

```sh
make examples
# out/gradient.png  out/plasma.png  out/mandelbrot.png  out/noise.png  out/cat.png  out/earth.png
```

Чёрная дыра отдельно (360 шагов интегрирования геодезик на пиксель):

```sh
make blackhole          # out/blackhole_960.png, 960x540, ~40 s
make blackhole-hq       # CPU 8 threads: 1920x1080 -> 1280x720 + 960x540, ~50 s
make blackhole-vk       # Vulkan Mali: 1920x1080 2x SSAA -> out/blackhole_vk.png, ~0.5 s
make blackhole-vk-video # Vulkan Mali: 5 s 1920x1080 H.264 MP4 -> out/blackhole_vk.mp4, ~10 s
```

Видео из сцены или PNG-секвенции:

```sh
./build/mathgfx video tests/anim.mgf out/anim.mp4 --fps 25 --crf 20
./build/mathgfx mux -o out/anim.mp4 --fps 25 --crf 20 out/frame_*.png
# fallback без x264: .avi + MJPEG
./build/mathgfx video tests/anim.mgf out/anim.avi --fps 25
```

Проверка сцен без рендера:

```sh
./build/mathgfx check examples/*.mgf
```

Тесты:

```sh
make test
```

---

## Что уже есть

- Лексер, парсер и однопроходный компилятор DSL в байткод.
- Регистровая VM: `r0..r15`, `double`, отдельный стек выражений.
- Выражения с приоритетами, унарный минус, сравнения, `^`.
- Встроенные переменные `X Y U V T W H PI`.
- Математика: `sin cos tan sqrt abs floor ceil fract min max pow mod
  atan2 clamp lerp smoothstep noise2 fbm2`.
- Блоки `if / elif / else / end`, циклы `repeat / break`,
  `y_loop`, `x_loop`, `time_loop`.
- `set_pixel`, `pixel`, `save_frame` с `%d` / `%03d`.
- PNG через `stb_image_write.h` (single-header, без внешних зависимостей).
- pthreads: `y_loop` делится по строкам, `--threads N` (по умолчанию —
  число CPU); `save_frame` остаётся однопоточным.
- Headless Vulkan compute: `src/vk_shader.c` — универсальный host,
  `examples/vulkan/<name>/<name>.comp` — отдельный шейдер на эффект. `mathgfx-vk
  --shader build/<name>.spv`; без окна и surface. На Mali-G615 1080p
  2×SSAA для black hole — ~0.1 с, весь вызов с PNG — ~0.5 с.
- Встроенное видео: H.264/MP4 через `libx264` + минимальный
  ISO BMFF-муксер (`src/mp4.c`), и MJPEG/AVI fallback (`src/avi.c`).
  `src/mux.c` сшивает PNG-секвенцию, `mathgfx video` рендерит сцену
  прямо в writer. Без ffmpeg.
- Восемь пример-сцен (SDF-кот, чёрная дыра, Земля и циклическая анимация облаков) и smoke-тесты, включая проверку структуры PNG.
- POSIX-sh линтер сцен `tools/scene-lint.sh`.

---

## Язык MGF

Файлы сцен — `.mgf`. Комментарии: `;` или `//` до конца строки.
Регистр имён важен: `r0`, `X`, `sin`, `set_pixel`.

### canvas

```asm
canvas 1280, 720
```

Обязательная строка, одна на сцену. Ширина и высота — целые литералы
в диапазоне `1..16384`; размер можно переопределить из CLI
(`--width`, `--height`) — удобно для быстрых превью и тестов.

### Переменные

| Имя  | Значение |
|------|----------|
| `X`, `Y` | координаты текущего пикселя, `0..W-1`, `0..H-1` |
| `U`, `V` | `(X + 0.5) / W`, `(Y + 0.5) / H` — центры пикселей в `[0,1]` |
| `T` | текущее время; `0` для статичной сцены |
| `W`, `H` | размер canvas |
| `PI` | 3.141592653589793 |

### Регистры

`r0..r15`, значения — `double`. Регистры **не сбрасываются на каждом
пикселе**: это глобальные переменные VM. Если регистр используется в
пикселе, его нужно явно установить (или он будет хранить значение с
предыдущего пикселя). Стартовое значение — `0.0`.

```asm
mov r0, U
mul r0, 2.0          ; r0 = r0 * 2.0
add r0, 1.0
```

### Выражения

Операнды инструкций могут быть выражениями — компилятор сам раскладывает
их во временные слоты:

```asm
mov r0, 0.5 + 0.5 * sin(U * 8.0 + T * 1.3)
```

Приоритеты (низкий → высокий):

1. сравнения: `< <= > >= == !=` (результат `1.0` / `0.0`)
2. `+ -`
3. `* / %`
4. унарные `+ -`
5. `^` (правоассоциативный)

`-2^2` = `-(2^2)` = `-4`. `%` — остаток (`fmod`).

Функции:

| Функция | Арность | Результат |
|---------|---------|-----------|
| `sin cos tan` | 1 | радианы |
| `sqrt abs floor ceil fract` | 1 | `fract(x) = x - floor(x)` |
| `min(a,b) max(a,b)` | 2 | — |
| `pow(a,b) mod(a,b)` | 2 | — |
| `atan2(y,x)` | 2 | радианы |
| `clamp(x,a,b)` | 3 | `min(max(x,a),b)` |
| `lerp(a,b,t)` | 3 | `a + (b-a)*t` |
| `smoothstep(a,b,t)` | 3 | плавная интерполяция `a..b` |
| `noise2(x,y)` | 2 | value noise, `[0,1)` |
| `fbm2(x,y,octaves)` | 3 | fbm, `[0,1]`; octaves 1..10 |

Запятые внутри вызова функции обязательны: `min(a, b)`.

### Пиксели

```asm
set_pixel X, Y, r0, r1, r2   ; явные координаты
pixel r0, r1, r2             ; текущие X, Y
```

Координаты берутся `floor(x)`, `floor(y)`. Выход за границы canvas
игнорируется. Цвет — три `double`, обрезаются в `[0,1]` и округляются в
8 бит. Альфа пока не поддерживается.

### Циклы

```asm
y_loop
  x_loop
    ...
  end
end
```

`y_loop` перебирает `Y = 0..H-1`, `x_loop` — `X = 0..W-1`.
Альтернатива — `pixel` внутри `x_loop`.

`repeat`:

```asm
repeat 64
  ...
  if r7 > 4.0
    break
  end
end
```

Счётчик вычисляется один раз, приводится к `floor`, ограничен 10 000 000.
`break` работает в `repeat`, `y_loop`, `x_loop`, `time_loop`.

`time_loop`:

```asm
time_loop 0.0, 10.0, 0.04   ; from (включительно), to (не включая), step
  ...
  save_frame "out/frame_%04d.png"
end
```

`T` меняется от `from` с шагом `step`, пока `T < to`. `step` должен быть
`> 0`. Без `save_frame` останется только последний кадр в памяти.

Для статичной сцены время можно задать вручную:

```asm
time 1.25
```

### Условия

```asm
if U < 0.5
  mov r0, 1.0
elif U < 0.75
  mov r0, 0.5
else if U < 0.9
  mov r0, 0.25
else
  mov r0, 0.0
end
```

`elif` и `else if` — одно и то же; вся цепочка закрывается одним `end`.
Обычный вложенный `if` на новой строке требует своего `end`.

### save_frame

```asm
save_frame "out/frame_%04d.png"
```

Пишет текущий кадр в PNG. `%d`, `%02d`, `%04d` заменяются на номер
кадра (счётчик `save_frame`, начиная с 0). Каталоги создаются
автоматически. Исторический вариант `save_frame T, "path"` тоже
принимается — выражение перед строкой игнорируется.

Если сцена не вызывает `save_frame`, CLI пишет финальный кадр в
`out.png` (или в путь, переданный вторым аргументом `render`).

---

## Пример сцены

`examples/plasma.mgf`:

```asm
canvas 1280, 720

y_loop
  x_loop
    mov r0, 0.5 + 0.5 * sin(U * 8.0 + T * 1.3 + noise2(U * 3.0, V * 3.0) * 2.0)
    mov r1, 0.5 + 0.5 * cos(V * 9.0 - T * 0.9 + fbm2(U * 2.0, V * 2.0, 4.0) * 3.0)
    mov r2, 0.5 + 0.5 * sin((U + V) * 7.0 + T * 0.7)
    set_pixel X, Y, r0, r1, r2
  end
end
```

`examples/cat.mgf` — портрет кота из SDF-масок: эллипсы (голова, тело,
глаза), треугольники (уши, нос), капсулы (усы, рот), аналитический
антиалиасинг через `r9 = 1.5 / H`, полосы и шум на шерсти. Это уже не
«градиент», а полноценная композиция на ~1800 инструкций на пиксель.

`examples/earth.mgf` — процедурная Земля с атмосферой, биомами, облаками
и ночными огнями; `examples/earth_clouds.mgf` — 12-секундный циклический
пролёт облаков (CPU `time_loop` + `save_frame`, `make earth-video`).

---

## Чёрная дыра

`examples/blackhole.mgf` — рейтрейсинг шварцшильдовской чёрной дыры
(`rs = 1`, `c = 1`). На пиксель: камера на `r = 24 rs`, наклон 12° над
плоскостью диска, вертикальный half-FOV 18°.

Луч интегрируется leapfrog-шагом по уравнению нулевой геодезики в
картезианской форме:

```text
a = -1.5 * h^2 * r_vec / r^5        h = |r x v| = const
```

Это эквивалент уравнения `u'' + u = 3 M u^2` при `M = 0.5`. Условия
остановки:

- `r < 1` — горизонт, чёрный;
- пересечение плоскости `z = 0` в кольце `3..12 rs` — непрозрачный
  тонкий диск;
- `r > 40` и луч движется наружу — процедурный фон (звёзды + туманность).

Диск: кеплеровское вращение, фактор Доплера
`D = 1 / (1 - beta * v_los)`, `beta = sqrt(0.5 / r)`, гравитационное
красное смещение `sqrt(1 - 1/r)`, яркость `~D^3`, fbm-турбулентность,
горячий внутренний ореол и мягкий внешний край. Диск бесконечно
тонкий и оптически толстый: берётся первое пересечение плоскости;
многократные изображения высших порядков не трассируются поштучно.
Фотонное кольцо `b_c = 3*sqrt(3)/2 ~ 2.598 rs` поэтому дополнительно
усилено гладкой компонентой — иначе пошаговое сэмплирование оставляет
его пунктирным.

360 шагов на луч, ~920 инструкций в сцене.

- CPU: 1920×1080 — ~154 s в один поток, ~45–50 s на 8 потоках (pthreads по
  строкам). `make blackhole-hq` делает CPU-рендер и Lanczos-даунскейл
  до 1280×720/960×540.
- Vulkan compute (Mali-G615): 1920×1080 с 2×SSAA — ~0.1 s на GPU,
  ~0.5 s с учётом readback и PNG. Итог: `out/blackhole_vk.png`.
  `examples/vulkan/blackhole/blackhole.comp` — та же модель, переписанная на GLSL float32;
  железных RT-ядер нет, это compute-рейтрейсинг.
- Видео: `make blackhole-vk-video` рендерит 5 секунд 1920×1080
  (125 кадров, 25 fps) через тот же шейдер с `time` и пишет
  `out/blackhole_vk.mp4` (~9.2 s, ~1.7 MB, H.264). MJPEG/AVI fallback —
  `make blackhole-vk-video-avi` (~9.5 s, ~15 MB).

---

## Vulkan: один шейдер на сцену

`mathgfx-vk` намеренно не компилирует MGF и не притворяется общим
GPU-бэкендом. Каждая тяжёлая сцена — отдельный compute-шейдер
(`blackhole.comp`, `dof_demo.comp`, ...):

```
examples/vulkan/blackhole/blackhole.comp  ->  build/blackhole.spv  ->  mathgfx-vk --shader ...
```

Интерфейс шейдера:

- `binding 0`: storage buffer `uint pixels[]`, packed `0x00BBGGRR`;
- push constants (16 байт): `uint width, height; float time; uint frame`;
- локальная группа 8×8, один invocation на пиксель.

Добавить новый эффект:

```sh
# 1. написать examples/vulkan/foo/foo.comp
# 2. собрать SPIR-V
glslangValidator -V -o build/foo.spv examples/vulkan/foo/foo.comp
# 3. запустить тем же host-ом
./build/mathgfx-vk --shader build/foo.spv --width 1920 --height 1080 \
    --out out/foo.png
```

Для двух текущих шейдеров в Makefile есть явные правила
(`build/blackhole.spv`, `build/dof_demo.spv`). Для нового эффекта
достаточно собрать SPIR-V через `glslangValidator` и запустить тот же
`mathgfx-vk --shader build/foo.spv ...`. Host не меняется.

---

## DoF: честная тонкая линза

`mathgfx-vk` умеет настоящий Depth of Field, а не экранный blur:

```sh
--dof N        число сэмплов линзы на пиксель
--aperture A   радиус апертуры в rs (0 = pinhole)
--focus F      расстояние до фокальной плоскости в rs
```

Для каждого сэмпла луч стартует в случайной точке диска апертуры и
направляется в фокальную плоскость; дальше — та же геодезика, диск и
фотонное кольцо. Никакого depth-буфера и пост-блюра.

```sh
# 1080p, 64 сэмпла, мягкое киношное DoF
make blackhole-dof

# вручную
./build/mathgfx-vk --shader build/blackhole.spv \
    --width 1920 --height 1080 --ssaa 1 --gpu 0 \
    --dof 64 --aperture 0.4 --focus 24 --out out/blackhole_dof.png
```

Результат: `out/blackhole_dof.png` (64 samples, aperture 0.4, focus
24 rs, ~1.8 s GPU / ~2.5 s total). `out/blackhole_dof_wide.png` —
вариант с aperture 0.8, сильнее размытие.

Физическая оговорка: на астрофизических масштабах DoF почти нулевой;
`aperture 0.4–1.0 rs` при `focus 24 rs` — это киношная камера, а не
реальный телескоп. Модель камеры при этом честная: тонкая линза,
N независимых лучей, без фейкового пост-процесса. Для видео 8–16
сэмплов дают мягкий фон, но может быть лёгкий шум; для стилла лучше
64–128. Для видео есть цель `make blackhole-dof-video` (8 samples,
125 кадров, ~32 s).

---

## DoF-демо: боке-сферы

Специальный пример, где DoF читается сразу:

```sh
make dof-demo        # out/dof_demo.png, 1920x1080, 256 samples, ~1.5 s
make dof-demo-video  # out/dof_demo.mp4, 120 кадров, 32 samples, focus pull, ~19 s
```

Сцена: резкая янтарная сфера на фокусной плоскости, уходящий ряд
глянцевых сфер, эмиссивные орбы на разных глубинах (их изображения
превращаются в боке), глянцевый пол с сеткой, контактными тенями и
отражениями. Камера — та же честная тонкая линза: `--dof 256
--aperture 0.32 --focus 5.0`. Никакого depth-буфера и пост-блюра.

В видео сферы и орбы двигаются, а фокус плавно уезжает с передней
сферы на задний план и возвращается (`--focus-end 9.0`). 120 кадров,
32 samples, 1920×1080, H.264, ~9.7 МБ, ~19 s. Стоп-кадр в дальнем фокусе:
`out/dof_demo_focus9.png`.

Параметры подобраны так, чтобы передняя сфера была резкой, а фон —
мягким; сетка на полу даёт очевидный depth cue, а орбы — крупное
боке. В отличие от чёрной дыры (где на астрофизических масштабах DoF
почти нет), здесь эффект — главный герой кадра.

Замеры: 256 samples 1080p — ~0.8 s GPU / ~1.5 s total; видео 32
samples, 125 кадров — 18.7 s (GPU 12.4 s, readback 1.5 s, encode 4.8 s),
~11 MB.

---

## Видео без ffmpeg

В системе два встроенных видео-пути:

- **H.264/MP4** — `libx264` + собственный ISO BMFF/MP4-муксер
  (`src/mp4.c`); качество задаётся `--crf` (0..51, меньше — лучше).
- **MJPEG/AVI** — JPEG-кадры через `stb_image_write` + RIFF/AVI-муксер
  (`src/avi.c`); качество задаётся `--quality` (1..100).

Никакого `ffmpeg`, `popen` или внешних контейнеров. Для MP4 нужен
`libx264` (`pkg install libx264`); AVI-путь работает без него.

```sh
# 1. MP4 из сцены с time_loop + save_frame (рекомендуется для Android)
./build/mathgfx video tests/anim.mgf out/anim.mp4 --fps 25 --crf 20

# 2. сшить PNG-секвенцию в MP4
./build/mathgfx mux -o out/anim.mp4 --fps 25 --crf 20 out/frame_*.png

# 3. Vulkan-анимация: time = --time + frame/fps
./build/mathgfx-vk --shader build/blackhole.spv \
    --width 1920 --height 1080 --ssaa 1 --gpu 0 \
    --video out/blackhole_vk.mp4 --frames 125 --fps 25 --crf 20

# fallback: AVI/MJPEG
./build/mathgfx video tests/anim.mgf out/anim.avi --fps 25 --quality 90
```

Для `mathgfx video` путь в `save_frame` игнорируется — кадр уходит в
writer, а не в PNG.

Почему AVI мог открыться чёрным: файл был валидным MJPEG/AVI (кадры
читаются), но Android-плеер часто не поддерживает MJPEG/AVI. MP4/H.264
играется нативно. Размеры одного и того же 5-секундного 1080p-клипа:
MP4 — ~1.7 MB, AVI/MJPEG — ~15 MB.

Ограничения: звука нет; HEVC/VP9/AV1 и аппаратный видео-энкодер пока
не подключены. MJPEG остаётся fallback-ом и хорошо подходит для VLC/mpv.

---

## CLI

```
mathgfx render <scene.mgf> [out.png] [--width N] [--height N] [--threads N]
mathgfx check <scene.mgf> [more.mgf ...]
mathgfx video <scene.mgf> <out.mp4|out.avi> [--fps N] [--crf N]
                [--quality N] [--width N] [--height N]
mathgfx mux -o <out.mp4|out.avi> [--fps N] [--crf N] [--quality N]
                <frame.png> ...
mathgfx --version

mathgfx-vk [--shader build/blackhole.spv] [--width N] [--height N]
           [--ssaa N] [--gpu N] [--time SECONDS]
           [--dof N] [--aperture A] [--focus F]
           [--out out/blackhole_vk.png]
           [--video out.mp4|out.avi] [--frames N] [--fps N]
           [--crf N] [--quality N] [--list-gpus]
```

`render` печатает время и число потоков. Без `--threads` берётся
`nproc` (до 64); сцены с `save_frame` и без `y_loop` рендерятся в один
поток. `check` компилирует сцену и показывает размер, число инструкций
и строковых констант.

`video` рендерит `time_loop` + `save_frame` прямо в writer: пути
`save_frame` игнорируются, кадры уходят в MP4 или AVI по расширению.
`mux` сшивает готовые PNG в тот же формат. `mathgfx-vk` — отдельный
headless Vulkan compute host; он запускает любой совместимый SPIR-V
(`--shader`), `--ssaa N` рендерит в N× разрешении и усредняет при
readback. С `--video` он рендерит N кадров с меняющимся `time` и пишет
MP4 или AVI по расширению.

---

## Архитектура

```
scene.mgf
   │
   ▼
lexer.c ── tokens ──▶ parser.c ── Instr[] ──▶ vm.c ── RGB8 ──┬──▶ stb_image_write ──▶ PNG
                              (one pass)         │           │
                                                 │           └──▶ video.c ──┬──▶ avi.c ──▶ AVI
                                                 │                          └──▶ mp4.c ──▶ MP4
                                                 └── save_frame / time_loop
```

- `src/lexer.c` — токены, позиции `line:col`, строковые литералы.
- `src/parser.c` — рекурсивный спуск, сразу генерирует байткод
  (AST не строится), патчит прыжки блоков.
- `src/vm.c` — регистры `double`, стек выражений, циклы, шум, запись
  пикселей и кадров, pthreads-рендер по строкам, frame-sink для видео.
- `src/png.c` — PNG/JPEG через `third_party/stb_image_write.h`,
  `mkdir -p` для путей кадров.
- `src/avi.c` — RIFF/AVI-муксер и JPEG-кодирование кадров.
- `src/mp4.c` — H.264 через libx264 + ISO BMFF/MP4-муксер.
- `src/video.c` — выбор writer-а по расширению `.avi`/`.mp4`.
- `src/mux.c` — PNG-секвенция → AVI/MP4 (stb_image).
- `src/main.c` — CLI, включая `video` и `mux`.

GPU-путь для чёрной дыры:

```
examples/vulkan/blackhole/blackhole.comp ── glslangValidator ──▶ build/blackhole.spv
        (float32-порт blackhole.mgf)                       │
                                                           ▼
                           mathgfx-vk --shader build/blackhole.spv
                                                           │
                                                           ▼
                              Vulkan compute -> readback -> box SSAA
                                                           │
                                                           ▼
                                              PNG / MP4 / AVI
```

- `examples/vulkan/blackhole/blackhole.comp` — GLSL compute 8×8, float32; ручной порт той
  же модели: геодезики, диск и фотонное кольцо. Из `.mgf` не
  генерируется — это отдельный GPU-бэкенд, а не общий DSL-компилятор.
- `src/vk_shader.c` — универсальный headless Vulkan host: instance →
  physical device → compute queue → storage buffer → dispatch → readback
  → PNG или AVI. Он не знает про чёрную дыру; конкретный эффект задаётся
  `--shader`.
- Сборка требует `vulkan-loader`, `vulkan-headers` и `glslangValidator`
  (в Termux: `pkg install vulkan-loader vulkan-headers vulkan-tools glslang`).
  Для Mali используется `vulkan-wrapper-android`; для софтверного
  fallback — `mesa-vulkan-icd-swrast` (lavapipe).
- `--gpu` выбирает физическое устройство; `--list-gpus` показывает
  Mali и lavapipe.

Почему ядро на C:

- per-pixel математике нужны `double`, `sin/cos/sqrt` и предсказуемая
  скорость; C даёт это без fixed-point-прослоек;
- ядро остаётся маленьким и переносимым: C99 + `-lm` + `-pthread`;
- Vulkan-бэкенд подключается отдельным бинарником и не тянет за собой
  софтверный путь.

Тулинг — обычный POSIX sh: `tools/scene-lint.sh` проверяет `canvas` и
баланс блоков без интерпретатора и внешних зависимостей.

---

## SH-линтер

```sh
make lint
# или
sh tools/scene-lint.sh examples
sh tools/scene-lint.sh tests
```

Проверяет для каждого `*.mgf`:

- есть ли `canvas`;
- баланс открывающих блоков (`if`, `repeat`, `y_loop`, `x_loop`,
  `time_loop`) и `end`;
- печатает `lines`, `blocks`, `pixels`, `canvas` и `OK/WARN`.

Код возврата — `0`, если все сцены чистые, иначе `1`.

---

## Тесты

```sh
make test
```

`tests/run.sh`:

- компилирует все примеры;
- рендерит их в 64×36 и проверяет PNG через `tests/check_png.py`
  (сигнатура, IHDR, zlib-поток, точная длина распакованных строк);
- проверяет цвета всех четырёх веток `if/elif/else if/else` через
  `tests/check_pixels.py` (полный PNG-анфильтр, сверка RGB);
- проверяет, что рендер в 1 поток и в 8 потоков даёт побайтово
  одинаковый PNG (детерминизм строк);
- проверяет `time_loop` + `save_frame` (ровно 3 кадра `frame_000..002`);
- проверяет `mathgfx video` и `mathgfx mux`:
  - MP4: `ftyp`/`moov`/`mdat`, `avc1`/`avcC`, SPS/PPS, `stsz`/`stco`/
    `stts`/`stsc`/`stss`, длины NAL — `tests/check_mp4.py`;
  - AVI: RIFF/AVI, MJPG, `movi`, `idx1`, декодирование JPEG-кадров через
    `tests/check_avi.py` (PIL);
- проверяет, что битые сцены (`bad_syntax`, `bad_time`) отвергаются;
- проверяет override размеров через CLI.

Отдельно: `make test-vk` — Vulkan PNG + Vulkan MP4 + Vulkan AVI.

---

## Производительность

Замеры на Termux/aarch64 (Mali-G615, 8 CPU), включая PNG/JPEG/H.264-кодирование.
CPU-цифры — если не указано иное, в один поток:

| Сцена | Размер | Время |
|-------|--------|-------|
| gradient | 1280×720 | ~0.10 s |
| plasma | 1280×720 | ~0.38 s |
| noise | 1280×720 | ~0.66 s |
| mandelbrot (96 итераций) | 960×540 | ~1.7 s |
| cat (SDF-портрет) | 1280×720 | ~9.8 s |
| earth (планета) | 1280×720 | ~2.4 s (8 потоков) |
| blackhole (360 шагов/луч) | 640×360 | ~17 s (1 поток) |
| blackhole | 960×540 | ~40 s (1 поток) / ~12 s (8) |
| blackhole | 1920×1080 | ~154 s (1 поток) / ~45–50 s (8) |
| blackhole Vulkan 2×SSAA | 1920×1080 | ~0.1 s GPU / ~0.5 s total |
| blackhole Vulkan video, 125 кадров, MP4 | 1920×1080 | ~9.2 s, ~1.7 MB |
| blackhole Vulkan video, 125 кадров, AVI | 1920×1080 | ~9.5 s, ~15 MB |
| blackhole Vulkan DoF, 64 samples | 1920×1080 | ~1.8 s GPU / ~2.5 s total |
| blackhole Vulkan DoF video, 8 samples | 1920×1080 | ~32 s (125 кадров) |
| DoF demo still, 256 samples | 1920×1080 | ~0.8 s GPU / ~1.5 s total |
| DoF demo video, 32 samples, focus pull | 1920×1080 | ~19 s (120 кадров), ~9.7 MB |

Узкие места CPU: интерпретация байткода на пиксель и PNG/JPEG-компрессия.
pthreads уже включён; следующий шаг — встроенный SSAA CPU. Vulkan остаётся
набором per-scene шейдеров, а не общим DSL-бэкендом.

---

## Ограничения v0.1

- Видео: H.264/MP4 и MJPEG/AVI. Звука, HEVC/VP9/AV1 и аппаратного
  видео-энкодера пока нет; MJPEG остаётся fallback-ом.
- Нет загрузки PNG-текстур и сэмплинга (декодер stb_image уже есть для
  `mux`, но `sample(u,v)` в DSL ещё не проброшен).
- Нет alpha-канала, HDR и гамма-коррекции.
- pthreads есть, но `save_frame`/видео-путь идёт в один поток.
- Нет include-файлов сцен и строковых переменных.
- Нет JIT.
- Vulkan-host универсален по SPIR-V, но шейдеры пишутся на каждый
  эффект; общего MGF→GPU компилятора нет. GPU-код использует float32,
  без float64 и hardware RT.

---

## Roadmap

1. Больше встроенных форматов: звук, HEVC/VP9/AV1, GIF/APNG.
2. Встроенный SSAA в CPU-путь (сейчас снаружи: рендер 2× + Lanczos).
3. `sample(u, v)` для PNG-текстур (stb_image).
4. Больше математики: `log exp sign step smoothstep` для векторов,
   gradient noise, curl noise.
5. JIT-компиляция байткода в C или прямой threaded-code интерпретатор.
6. Новые per-scene Vulkan-шейдеры: Kerr, джеты, планеты, туманности.
7. Расширение shell-тулинга: генератор сцен, статистика, проверка
   палитр.

---

## Структура

```
MathGFX/
├── include/mgfx.h            публичный API, опкоды, лимиты
├── src/
│   ├── internal.h            токены и лексер
│   ├── lexer.c
│   ├── parser.c              парсер + компилятор в байткод
│   ├── vm.c                  VM, pthreads, frame-sink
│   ├── png.c                 PNG/JPEG + mkdir -p
│   ├── avi.c / avi.h         MJPEG AVI muxer
│   ├── mp4.c / mp4.h         H.264/MP4 (libx264 + muxer)
│   ├── video.c / video.h     AVI/MP4 writer selection
│   ├── mux.c / mux.h         PNG sequence -> AVI/MP4
│   ├── vk_shader.c           headless Vulkan compute host
│   ├── stb_write_impl.c      stb_image_write implementation
│   └── main.c                CLI (render/check/video/mux)
├── examples/
│   ├── *.mgf                 CPU DSL scenes
│   └── vulkan/
│       ├── README.md
│       ├── run_all.sh
│       ├── blackhole/        blackhole.comp + run_*.sh + preview.jpg
│       └── dof_demo/         dof_demo.comp + run_*.sh + preview*.jpg
├── third_party/stb_image_write.h
├── third_party/stb_image.h
├── tests/
│   ├── run.sh
│   ├── check_png.py
│   ├── check_pixels.py
│   ├── check_avi.py
│   ├── check_mp4.py
│   ├── anim.mgf
│   ├── branches.mgf
│   ├── bad_syntax.mgf
│   └── bad_time.mgf
├── tools/scene-lint.sh       POSIX-sh линтер сцен
├── Makefile
├── README.md
└── Project.md
```

`stb_image_write.h` и `stb_image.h` — public domain / MIT (Sean Barrett),
лежат в `third_party/` как single-header. H.264-энкодер — системный
`libx264` (`pkg install libx264`).
