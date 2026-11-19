CC      ?= gcc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wshadow
CFLAGS  += -D_POSIX_C_SOURCE=200809L -Iinclude -Ithird_party -pthread
LDLIBS  ?= -lm -pthread -lx264

BUILD   := build
ifeq ($(OS),Windows_NT)
EXE        := .exe
VULKAN_LIB := -lvulkan-1
else
EXE        :=
VULKAN_LIB := -lvulkan
endif
BIN     := $(BUILD)/mathgfx$(EXE)
VK_BIN  := $(BUILD)/mathgfx-vk$(EXE)
SRCS    := src/main.c src/lexer.c src/parser.c src/vm.c src/png.c src/avi.c src/mux.c src/mp4.c src/video.c src/portable.c src/stb_write_impl.c
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
VK_OBJ  := $(BUILD)/vk_shader.o
SPV     := $(BUILD)/blackhole.spv
DOF_SPV := $(BUILD)/dof_demo.spv
BH_SHADER  := examples/vulkan/blackhole/blackhole.comp
DOF_SHADER := examples/vulkan/dof_demo/dof_demo.comp
HDRS    := include/mgfx.h src/internal.h src/avi.h src/mux.h src/mp4.h src/video.h src/portable.h third_party/stb_image_write.h third_party/stb_image.h

.PHONY: all clean test test-vk examples lint blackhole blackhole-hq blackhole-vk blackhole-vk-video blackhole-vk-video-avi blackhole-dof blackhole-dof-video dof-demo dof-demo-video vulkan-examples earth earth-video

all: $(BIN) $(VK_BIN) $(SPV) $(DOF_SPV)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/%.o: src/%.c $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SPV): $(BH_SHADER)
	@mkdir -p $(BUILD)
	glslangValidator -V -o $@ $<

$(DOF_SPV): $(DOF_SHADER)
	@mkdir -p $(BUILD)
	glslangValidator -V -o $@ $<

$(BUILD)/vk_shader.o: src/vk_shader.c $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(VK_BIN): $(VK_OBJ) $(BUILD)/png.o $(BUILD)/avi.o $(BUILD)/mp4.o $(BUILD)/video.o $(BUILD)/portable.o $(BUILD)/stb_write_impl.o
	$(CC) $(CFLAGS) -o $@ $^ $(VULKAN_LIB) $(LDLIBS)

test: $(BIN)
	sh tests/run.sh

examples: $(BIN)
	@mkdir -p out
	./$(BIN) render examples/gradient.mgf out/gradient.png
	./$(BIN) render examples/plasma.mgf out/plasma.png
	./$(BIN) render examples/mandelbrot.mgf out/mandelbrot.png
	./$(BIN) render examples/noise.mgf out/noise.png
	./$(BIN) render examples/cat.mgf out/cat.png
	./$(BIN) render examples/earth.mgf out/earth.png

earth: $(BIN)
	@mkdir -p out
	./$(BIN) render examples/earth.mgf out/earth.png

earth-video: $(BIN)
	@mkdir -p out
	./$(BIN) video examples/earth_clouds.mgf out/earth_clouds.mp4 --fps 25 --crf 20

blackhole: $(BIN)
	@mkdir -p out
	./$(BIN) render examples/blackhole.mgf out/blackhole_960.png --width 960 --height 540

blackhole-hq: $(BIN)
	@mkdir -p out
	./$(BIN) render examples/blackhole.mgf out/blackhole_1080.png --width 1920 --height 1080
	magick out/blackhole_1080.png -filter Lanczos -resize 1280x720 out/blackhole.png
	magick out/blackhole_1080.png -filter Lanczos -resize 960x540 out/blackhole_960.png

blackhole-vk: $(VK_BIN) $(SPV)
	sh examples/vulkan/blackhole/run_still.sh

blackhole-vk-video: $(VK_BIN) $(SPV)
	sh examples/vulkan/blackhole/run_video.sh

blackhole-vk-video-avi: $(VK_BIN) $(SPV)
	sh examples/vulkan/blackhole/run_video_avi.sh

blackhole-dof: $(VK_BIN) $(SPV)
	sh examples/vulkan/blackhole/run_dof.sh

blackhole-dof-video: $(VK_BIN) $(SPV)
	sh examples/vulkan/blackhole/run_dof_video.sh

dof-demo: $(VK_BIN) $(DOF_SPV)
	sh examples/vulkan/dof_demo/run_still.sh

dof-demo-video: $(VK_BIN) $(DOF_SPV)
	sh examples/vulkan/dof_demo/run_video.sh

vulkan-examples: $(VK_BIN) $(SPV) $(DOF_SPV)
	sh examples/vulkan/run_all.sh

test-vk: $(VK_BIN) $(SPV) $(DOF_SPV)
	@mkdir -p out/test
	./$(VK_BIN) --shader $(SPV) --width 64 --height 36 --ssaa 1 --gpu 0 --out out/test/vk.png
	python3 tests/check_png.py out/test/vk.png 64 36
	./$(VK_BIN) --shader $(SPV) --width 64 --height 36 --ssaa 1 --gpu 0 \
		--dof 4 --aperture 0.4 --focus 24 --out out/test/dof.png
	python3 tests/check_png.py out/test/dof.png 64 36
	./$(VK_BIN) --shader $(DOF_SPV) --width 64 --height 36 --ssaa 1 --gpu 0 \
		--dof 4 --aperture 0.32 --focus 5.0 --out out/test/dof_demo.png
	python3 tests/check_png.py out/test/dof_demo.png 64 36
	./$(VK_BIN) --shader $(SPV) --width 64 --height 36 --ssaa 1 --gpu 0 \
		--video out/test/vk.mp4 --frames 3 --fps 25 --crf 20
	python3 tests/check_mp4.py out/test/vk.mp4 3 64 36
	./$(VK_BIN) --shader $(SPV) --width 64 --height 36 --ssaa 1 --gpu 0 \
		--video out/test/vk.avi --frames 3 --fps 25
	python3 tests/check_avi.py out/test/vk.avi 3 64 36

lint:
	sh tools/scene-lint.sh examples

clean:
	rm -rf $(BUILD) out
