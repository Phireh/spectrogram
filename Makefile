CC=g++
CFLAGS=-DIMGUI_IMPL_OPENGL_LOADER_GLEW -Wall -Wextra -Werror -O2 -Wno-unused-parameter 
LIBS=`pkg-config --libs glew` `sdl2-config --libs` -lSDL2_mixer -lFLAC
INCLUDES=`pkg-config --cflags glew` `sdl2-config --cflags` -I./imgui -I./imgui/backends -I./fft
IMGUI_SOURCES=$(wildcard imgui/*.cpp)
IMGUI_HEADERS=$(wildcard imgui/*.h)
IMGUI_OBJ=$(addsuffix .o,$(basename $(notdir $(IMGUI_SOURCES))))
IMGUI_BACKEND=imgui/backends/imgui_impl_sdl.cpp imgui/backends/imgui_impl_opengl3.cpp
IMGUI_BACKEND_OBJ=$(addsuffix .o,$(basename $(notdir $(IMGUI_BACKEND))))

spectrogram: spectrogram.cpp spectrogram.hpp $(IMGUI_OBJ) $(IMGUI_BACKEND_OBJ)
	@$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -o spectrogram $^ && echo "All good!" || rm spectrogram # Clean trash left behind by failed compile

%.o:imgui/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -c -o $@ $<

imgui_impl_sdl.o:imgui/backends/imgui_impl_sdl.cpp
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -c -o $@ $<

imgui_impl_opengl3.o:imgui/backends/imgui_impl_opengl3.cpp
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -c -o $@ $<

tags:
	@$(CC) $(CFLAGS) $(LIBS) $(INCLUDES) -M spectrogram.cpp | awk 'NR==1 { for (i=2; i<NF;++i) print $$i } NR>1 { for(i=1;i<NF;++i) print $$i }' | etags -L -


clean:
	rm $(IMGUI_OBJ) $(IMGUI_BACKEND_OBJ) spectrogram
