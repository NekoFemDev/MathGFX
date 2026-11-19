# PROJECT (MathGFX) — Software Procedural Renderer

## 1. Идея
Создание собственного низкоуровневого инструмента (CLI/библиотеки) для генерации 2D-изображений (PNG) и видео (MP4) с помощью математических вычислений. Основной путь — чистый софтверный рендеринг (Software Rendering) на CPU. Для тяжёлого финального кадра чёрной дыры добавлен опциональный headless Vulkan compute backend (без окна, graphics pipeline и RT-ядер).

## 2. Философия
- **Low-level**: Максимальный контроль над памятью и вычислениями. Никаких тяжелых зависимостей.
- **ASM-like DSL**: Собственный минималистичный язык для описания математики пикселя. 
- **No AI Slop**: Картинки генерируются алгоритмически, а не диффузионными моделями.
- **CPU reference + GPU compute**: CPU-путь остаётся эталонным; Vulkan-бэкенд ускоряет только тяжёлый рейтрейсинг чёрной дыры.

## 3. Архитектура
1. **Frontend (Язык)**: Парсер и лексер для ASM-like DSL. (Реализовано на C99.)
2. **Backend (VM/Интерпретатор)**: Выполняет инструкции, считает математику (sin, cos, sqrt, шумы) для каждого пикселя.
3. **Encoder**: 
   - Для PNG: Использование `stb_image_write.h` (single-header C library).
   - Для видео: H.264/MP4 — `libx264` + собственный ISO BMFF-муксер
     (`src/mp4.c`), без ffmpeg.
   - Fallback: MJPEG/AVI — JPEG-кадры через `stb_image_write` +
     RIFF/AVI-муксер (`src/avi.c`).
   - HEVC/VP9/AV1/звук — следующий этап.
4. **Vulkan compute backend**: GLSL compute-шейдер + headless host
   (`src/vk_blackhole.c`) для рейтрейсинга чёрной дыры. Собирается
   отдельным бинарником `build/mathgfx-vk`, CPU-путь не затрагивает.

## 4. Дизайн языка (ASM-like)
Язык должен быть регистровым или стековым. Примерная логика:
- `X`, `Y` (координаты пикселя), `T` (время для видео).
- Рендер происходит в цикле по `Y`, затем по `X`.
- Результат математики сохраняется в регистры цвета.

Пример синтаксиса (псевдо-ASM):
```asm
; Установить размер холста
canvas 1920, 1080

; Начало цикла по времени (для видео)
time_loop 0.0, 10.0, 0.04  ; from, to, step (25 fps)

    ; Начало цикла по Y и X
    y_loop 0, HEIGHT
        x_loop 0, WIDTH
            
            ; Математика цвета
            mov r0, X
            div r0, WIDTH      ; r0 = X / WIDTH (от 0.0 до 1.0)
            
            mov r1, Y
            div r1, HEIGHT     ; r1 = Y / HEIGHT
            
            ; Синусоидальная волна
            sin r2, X
            add r2, T          ; добавить время
            mul r2, 0.5
            add r2, 0.5        ; r2 = цвет в диапазоне 0..1
            
            ; Установить пиксель (X, Y) с цветом (r0, r1, r2)
            set_pixel X, Y, r0, r1, r2

        end x_loop
    end y_loop

    ; Сохранить кадр
    save_frame T, "out/frame_%d.png"

end time_loop

; Скомпилировать в видео
compile_video "out/frame_%d.png", "output.mp4", 25
```

> Примечание v0.1: в реализации — `W`/`H` вместо `WIDTH`/`HEIGHT`,
> добавлены `U`/`V`, `if/elif/else`, `repeat/break`, выражения в
> операндах и `noise2`/`fbm2`. `save_frame` пишет PNG-секвенцию.
> Вместо `compile_video` — `mathgfx video scene.mgf out.mp4`
> (сцена сразу в H.264/MP4), `mathgfx mux -o out.mp4 frames/*.png`
> (сшивание PNG) или `.avi` для MJPEG-fallback.
> Актуальный справочник языка — `README.md`.

5. Стек технологий

· Язык реализации ядра: C99 + pthreads (максимальная производительность для циклов).
· GPU: Vulkan compute + GLSL (glslang), headless, без окна; один
  SPIR-V-шейдер на эффект, host общий.
· Видео: H.264/MP4 (libx264 + свой muxer) и MJPEG/AVI fallback, без ffmpeg.
· Тулинг: POSIX sh (линтер сцен, тесты).
· Сборка: Makefile.

6. Потенциал и юзкейсы

· Генерация бесшовных процедурных текстур для игр.
· Создание генеративного арта (фракталы, шумы Перлина).
· Тестирование математических алгоритмов в визуальной форме.
· Связка с LLM: ИИ пишет код на нашем DSL по текстовому описанию.

7. Текущий статус

☑ Идея зафиксирована.
☑ Написать парсер DSL.
☑ Реализовать циклы и математику.
☑ Прикрутить stb_image_write.
☑ Сделать рендер первого кадра.

8. v0.1 — что реализовано

Реализация: C99-ядро + shell-тулинг. Подробное описание языка, CLI и
архитектуры — в `README.md`.

· `src/lexer.c` — лексер `.mgf` (позиции `line:col`, комментарии `;` и `//`).
· `src/parser.c` — рекурсивный спуск и однопроходный компилятор в байткод;
  операнды-выражения, `if/elif/else`, `repeat/break`, `y_loop`, `x_loop`,
  `time_loop`.
· `src/vm.c` — VM с 16 `double`-регистрами, `sin/cos/sqrt/noise2/fbm2`,
  запись RGB8, `save_frame` и pthreads-рендер по строкам (`--threads`).
· `src/vk_shader.c` + `examples/vulkan/blackhole/blackhole.comp`, `examples/vulkan/dof_demo/dof_demo.comp` —
  headless Vulkan compute; host принимает любой `.spv`
  (`make blackhole-vk`, `make dof-demo`). Есть честный thin-lens DoF:
  `--dof N --aperture A --focus F` (`make blackhole-dof`).
· `src/mp4.c` + `src/avi.c` + `src/video.c` + `src/mux.c` — H.264/MP4,
  MJPEG/AVI, выбор writer-а и сшивание PNG-секвенций.
· `src/png.c` — PNG/JPEG через `third_party/stb_image_write.h`.
· `examples/` — gradient, plasma, mandelbrot, noise, cat (SDF-портрет),
  blackhole (рейтрейсинг геодезик + аккреционный диск), earth (планета),
  earth_clouds (циклическая анимация облаков).
· `tests/` — smoke-тесты и валидация структуры PNG.
· `tools/scene-lint.sh` — POSIX-sh линтер сцен (баланс блоков, canvas,
  статистика). Запуск: `make lint`.

Быстрый старт:

```sh
make
make examples
./build/mathgfx render examples/plasma.mgf out/plasma.png
make blackhole-vk
make blackhole-vk-video
make test
make test-vk
```

Видео уже встроено: `mathgfx video` рендерит `time_loop` + `save_frame`
прямо в H.264/MP4 (или MJPEG/AVI по расширению), `mathgfx mux` сшивает
PNG-секвенцию, а `mathgfx-vk --video` делает то же на Vulkan. Звук,
HEVC/VP9/AV1 — следующий этап.