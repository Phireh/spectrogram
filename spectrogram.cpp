#include "spectrogram.hpp"

ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.70f, 1.00f);
Mix_Music *sdl_mixer_sound;
SDL_AudioSpec custom_audio_fmt_desired;
float custom_audio_volume = 1000.0f;
int selected_audio_device_idx;
SDL_AudioDeviceID custom_audio_open_device;
SDL_AudioSpec custom_audio_fmt_obtained;
flac_client_data_t flac_client_data;
FLAC__StreamDecoder *flac_stream_decoder;
float volume = 1.0f;

spectrogram_data_t spectrogram_data;

// TODO: turn state into enums or bitmasks
bool playing = false;
bool song_ended = false;

/* OpenGL globals */
// TODO: Check if we need to save the rbo
unsigned int waveform_fbo_arr[MAX_CHANNELS];
unsigned int waveform_texture_arr[MAX_CHANNELS];
unsigned int waveform_rbo_arr[MAX_CHANNELS];

unsigned int waveform_program;

// TODO: Don't be a disgusting human being
const char *waveform_vertex_shader_source =
    "#version 150\n" \
    "in vec2 position;\n" \
    "void main() {\n" \
    "gl_Position = vec4(position, 0.0, 1.0);\n" \
    "}";

const char *waveform_fragment_shader_source =
    "#version 150\n" \
    "out vec4 out_color;\n" \
    "void main() {\n" \
    "out_color = vec4(1.0, 1.0, 1.0, 1.0);\n" \
    "}";

// TODO: Make this unique
const char *spectrogram_vertex_shader_source = waveform_vertex_shader_source;
const char *spectrogram_fragment_shader_source = waveform_fragment_shader_source;

unsigned int waveform_width = 400;
unsigned int waveform_height = 100;

char infoLog[512];

unsigned int build_shader(const char *source, int type)
{
    unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if(!success)
    {

        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        printf("Error: shader compilation failed: %s\n", infoLog);        
    }
    // TODO: Maybe better error handling?
    return shader;
}

unsigned int make_opengl_program(const char *vertex_shader_source, const char *fragment_shader_source)
{
    unsigned int vertex_shader = build_shader(vertex_shader_source, GL_VERTEX_SHADER);
    unsigned int fragment_shader = build_shader(fragment_shader_source, GL_FRAGMENT_SHADER);

    unsigned int program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);

    // print linking errors if any
    int success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if(!success)         // TODO: Maybe better error handling?
    {
        glGetProgramInfoLog(program, 512, NULL, infoLog);
        printf("Error: shader linking failed: %s\n", infoLog);
    }
    else
    {
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
    }    
    return program;
}

unsigned int make_waveform_program()
{
    // TODO: Maybe refactor this
    return make_opengl_program(waveform_vertex_shader_source, waveform_fragment_shader_source);
}

int init_opengl_texture(unsigned int *fbo, unsigned int *rbo, unsigned int *texture, unsigned int width, unsigned int height)
{
    
    glGenFramebuffers(1, fbo);
    
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);

    // Complete the framebuffer

    /* Create texture */
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
  
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);  

    /* Attach texture to framebuffer */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);  


    /* Create depth+stencil rbo attachment for framebuffer so OpenGL can do depth testing */
    glGenRenderbuffers(1, rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, *rbo); 
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);  
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    /* Attach rbo to framebuffer */    
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, *rbo);

    // Return to normal regardless of success
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);   
}

int init_waveform_texture(int channel)
{
    return init_opengl_texture(&waveform_fbo_arr[channel], &waveform_rbo_arr[channel], &waveform_texture_arr[channel], waveform_width, waveform_height);
}

void draw_waveform_to_texture(int channel)
{
    glViewport(0, 0, waveform_width, waveform_height);
    glBindFramebuffer(GL_FRAMEBUFFER, waveform_fbo_arr[channel]);
    // Rendering
    // TODO: check if we need to fuck with the viewport
    // TODO: For now we just clear the texture to a hideous color to make sure it works

//    glEnable(GL_PROGRAM_POINT_SIZE); // this is for using it in the shader
    glUseProgram(waveform_program);
    glClearColor(1.0f, 0.6f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (flac_client_data.total_samples)
    {
        // TODO: Maybe remove magic numbers?
        int npoints = flac_client_data.total_samples*2;
        static float *channel_vertex_pointers_arr[MAX_CHANNELS] = {};

        if (!channel_vertex_pointers_arr[channel])
        {
            channel_vertex_pointers_arr[channel] = (float*) malloc(npoints*sizeof(float));
        }            
        
        float *points = channel_vertex_pointers_arr[channel];
        int bytes_per_sample = flac_client_data.bits_per_sample/8;

        for (int i = 0; i < flac_client_data.total_samples; ++i)
        {
            // TODO: Generalize for non-flac16 stereo
            int byte_offset = i * bytes_per_sample;
            float x_coord = ((2.0f/(flac_client_data.total_samples)) * i) - 1.0f;


            FLAC__int16 sample = *((FLAC__int16*)(&flac_client_data.buffers[channel][byte_offset]));
            
            float y_coord = (float)-sample/MAX_INT16; 

            points[i*2] = x_coord;
            points[(i*2)+1] = y_coord;

        }

        unsigned int vao;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);

        unsigned int vbo;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, npoints*sizeof(float), points, GL_STATIC_DRAW);

        // Each position attrib is a vec2
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
        glEnableVertexAttribArray(0);       

        glDrawArrays(GL_POINTS, 0, npoints);
        
    }

    // Return framebuffer to normal
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

int init_spectrogram_data(spectrogram_data_t *spectrogram_data, flac_client_data_t *flac_client_data)
{
    int nsamples = flac_client_data->total_samples;
    if (nsamples)
    {
        // TODO: Stereo audio
        spectrogram_data->channels = flac_client_data->channels;
        // FFT radix 2 algorithm needs an input of size 2^N
        int padded_nsamples = upper_pow2(nsamples);
        spectrogram_data->padded_nsamples = padded_nsamples;
        spectrogram_data->in_real = (double*) calloc(padded_nsamples, sizeof(double));
        spectrogram_data->in_imag = (double*) calloc(padded_nsamples, sizeof(double));
        spectrogram_data->out_real = (double*) calloc(padded_nsamples, sizeof(double));
        spectrogram_data->out_imag = (double*) calloc(padded_nsamples, sizeof(double));

        if (spectrogram_data->in_real && spectrogram_data->in_imag && spectrogram_data->out_real && spectrogram_data->out_imag)
        {
            // Copy sample data to in_real
            for (int i = 0; i < nsamples; ++i)
            {
                // TODO: Make this work for non-16b audio
                FLAC__int16 sample = *((FLAC__int16*)&flac_client_data->buffers[LEFT_CHANNEL][i*2]);
                spectrogram_data->in_real[i] = sample;
            }
            return padded_nsamples;
        }
        else
        {
            printf("Error trying to allocate memory for FFT arrays\n");
        }
    }
    else
    {
        printf("Trying to init FFT data without audio sample data\n");
    }
    return 0;
}

int fft_rect(spectrogram_data_t *spectrogram_data)
{
    // TODO: Queue this as a threaded job to avoid hiccups
    fft(spectrogram_data->in_real, spectrogram_data->in_imag, spectrogram_data->padded_nsamples, spectrogram_data->out_real, spectrogram_data->out_imag);    
    return 1;
}

void restart_song()
{
    if (flac_client_data.total_samples > 0)
        flac_client_data.play_position = 0;
}

void rewind_song(float percent)
{
    printf("Rewinding to %f%%",percent*100);
    // Sanity checks
    if (percent > 1.0f) return; // No point in going past the end of the song
    if (percent < 0.0f) percent = 0.0f;

    if (flac_client_data.total_samples > 0)
    {
        int bytes_per_sample = flac_client_data.bits_per_sample/8;
        int target_sample = (int)((float) flac_client_data.total_samples * percent);
        target_sample -= (target_sample % bytes_per_sample);
        target_sample *= flac_client_data.channels;
        flac_client_data.play_position = target_sample;        
    }
}


FLAC__StreamDecoderWriteStatus flac_write_callback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client_data)
{

    flac_client_data_t *client_data_ptr = (flac_client_data_t*) client_data;
    
    if (frame->header.channels <= MAX_CHANNELS)
    {
        int samples_per_channel = frame->header.blocksize;
        int bytes_per_sample = (frame->header.bits_per_sample)/8;
        int bytes_per_channel = samples_per_channel * bytes_per_sample;

        // NOTE: A simple memcpy does not work here. Seems like the signed-ness of the samples needs handling with care.
        // took inspiration from https://github.com/NicolasR/SDL_mixer-1.2/blob/master/load_flac.c

        for (int i = 0; i < samples_per_channel; ++i)
        {
            FLAC__int16 i16;
            FLAC__uint16 ui16;

            i16 = (FLAC__int16)buffer[0][i];
            ui16 = (FLAC__uint16)i16;

            int offset = (i * bytes_per_sample) + (client_data_ptr->write_position);
            client_data_ptr->buffers[LEFT_CHANNEL][offset] = (char)(ui16);
            client_data_ptr->buffers[LEFT_CHANNEL][offset+1] = (char)(ui16 >> 8);

            i16 = (FLAC__int16)buffer[1][i];
            ui16 = (FLAC__uint16)i16;

            client_data_ptr->buffers[RIGHT_CHANNEL][offset] = (char)(ui16);
            client_data_ptr->buffers[RIGHT_CHANNEL][offset+1] = (char)(ui16 >> 8); 
            
        }
        
        
        printf("Processed %d samples per channel (%d bytes). Current write offset %d\n", samples_per_channel,
               bytes_per_channel, client_data_ptr->write_position);
        client_data_ptr->write_position += bytes_per_channel;
    }

    
    ++flac_client_data.flac_write_callbacks;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}


void flac_metadata_callback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
    // TODO: Make write callback for flac
    printf("Being called with metadata of %d bytes and type ", metadata->length);
         
     
    switch (metadata->type) {
    case FLAC__METADATA_TYPE_STREAMINFO:
        printf("FLAC__METADATA_TYPE_STREAMINFO\n");
        printf("min_blocksize: %d\n", metadata->data.stream_info.min_blocksize);
        printf("max_blocksize: %d\n", metadata->data.stream_info.max_blocksize);
        printf("min_framesize: %d\n", metadata->data.stream_info.min_framesize);
        printf("max_framesize: %d\n", metadata->data.stream_info.max_framesize);
        printf("sample_rate: %d\n", metadata->data.stream_info.sample_rate);
        printf("channels: %d\n", metadata->data.stream_info.channels);
        printf("bits_per_sample: %d\n", metadata->data.stream_info.bits_per_sample);
        printf("total_samples: %ld (per channel)\n", metadata->data.stream_info.total_samples);

        printf("md5sum: %02hhx %02hhx\n", metadata->data.stream_info.md5sum[0], metadata->data.stream_info.md5sum[1]);

        // Fill our client data struct with info about the stream
        ((flac_client_data_t*) client_data)->channels = metadata->data.stream_info.channels;
        ((flac_client_data_t*) client_data)->sample_rate = metadata->data.stream_info.sample_rate;
        ((flac_client_data_t*) client_data)->bits_per_sample = metadata->data.stream_info.bits_per_sample;
        ((flac_client_data_t*) client_data)->total_samples = metadata->data.stream_info.total_samples;


        if (metadata->data.stream_info.channels > MAX_CHANNELS)
        {
            printf("Error: FLAC file has %d channels, MAX is %d", metadata->data.stream_info.channels, MAX_CHANNELS);
        }
        else
        {         
            for (uint i = 0; i < metadata->data.stream_info.channels; ++i)
            {
                ((flac_client_data_t*) client_data)->buffers[i] = (unsigned char*) malloc(((metadata->data.stream_info.bits_per_sample/8) * metadata->data.stream_info.total_samples));
            }         
        }
        break;
    default:
        break;
    }
}

void flac_error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
    printf("FLAC ERROR: ");
    switch (status) {
    case FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC:
        printf("FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC: An error in the stream caused the decoder to lose synchronization.\n");
        break;        
    case FLAC__STREAM_DECODER_ERROR_STATUS_BAD_HEADER:
        printf("FLAC__STREAM_DECODER_ERROR_STATUS_BAD_HEADER: The decoder encountered a corrupted frame header.\n");
        break;
    case FLAC__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH:        
        printf("FLAC__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH: The frame's data did not match the CRC in the footer.\n");
        break;
    case FLAC__STREAM_DECODER_ERROR_STATUS_UNPARSEABLE_STREAM:
        printf("FLAC__STREAM_DECODER_ERROR_STATUS_UNPARSEABLE_STREAM: The decoder encountered reserved fields in use in the stream.\n");
        break;
    }
    ++flac_client_data.flac_error_callbacks;
}


FLAC__StreamDecoder *flac_decoder_init()
{
    FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();

    if (decoder)
    {
        
        FILE *sound_file_handle = fopen("sound/sound.flac", "rb");
        FLAC__stream_decoder_init_FILE(decoder, sound_file_handle, flac_write_callback, flac_metadata_callback, flac_error_callback, (void*)&flac_client_data);
        printf("Initialized FLAC decoder with state %s\n", FLAC__stream_decoder_get_resolved_state_string(decoder));
        FLAC__stream_decoder_process_until_end_of_metadata(decoder);

        FLAC__stream_decoder_process_until_end_of_stream(decoder);
        
        printf("Processed %d FLAC write callbacks\n", flac_client_data.flac_write_callbacks);
        printf("Wrote %d bytes per channel (%d samples)\n", flac_client_data.write_position, flac_client_data.write_position / (flac_client_data.bits_per_sample/8));

    }
    return decoder;
}

Mix_Music *load_sound_from_file(const char *filename)
{
    Mix_Music *sound = Mix_LoadMUS(filename);
    if (!sound)
    {
        printf("Error loading sound: %s\n", Mix_GetError());
    }
    return sound;
}

void mix_audio(void *unused, Uint8 *stream, int len)
{

    // TODO: Check for correct audio format
    
    int bytes_per_sample = flac_client_data.bits_per_sample/8;
    int channels = custom_audio_fmt_obtained.channels;
    int individual_samples = len / bytes_per_sample;  // individual samples are 16 int signed for each channel
    int stereo_samples = individual_samples/channels;             
    int desired_stereo_samples = stereo_samples;


    int total_stereo_samples = flac_client_data.total_samples;
    int curr_sample = flac_client_data.play_position/(bytes_per_sample);

    if (curr_sample + desired_stereo_samples > total_stereo_samples)
    {
        printf("Reached end of song. Fill the rest and pause audio.\n");
        stereo_samples = total_stereo_samples - curr_sample;
        song_ended = true;
    }


    // TODO: Check for address boundary error around here
    
    for (int i = 0; i < stereo_samples; ++i)
    {
        // Sample = L L R R
        int play_offset = i * bytes_per_sample + flac_client_data.play_position;
        int stream_offset = i * bytes_per_sample * channels;

        // Version with volume
        FLAC__int16 sample_L = *((FLAC__int16*)(&flac_client_data.buffers[LEFT_CHANNEL][play_offset]));
        FLAC__int16 sample_R = *((FLAC__int16*)(&flac_client_data.buffers[RIGHT_CHANNEL][play_offset]));

        sample_L = (FLAC__int16)((float)sample_L * volume);
        sample_R = (FLAC__int16)((float)sample_R * volume);

        *(FLAC__int16*)(stream + stream_offset) = sample_L;
        *(FLAC__int16*)(stream + stream_offset + sizeof(FLAC__int16)) = sample_L;
            
    }
    
    flac_client_data.play_position += stereo_samples * bytes_per_sample;
    
    for (int i = stereo_samples; i < desired_stereo_samples; ++i)
    {
        // Fill with silence if we don't have enough data to fill the audio buffer
        int stream_offset = i * bytes_per_sample * channels;
        *(stream + stream_offset)     = 0;
        *(stream + stream_offset + 1) = 0;
        *(stream + stream_offset + 2) = 0;
        *(stream + stream_offset + 3) = 0;
    }
}

SDL_AudioDeviceID sdl_custom_audio_init(const char *device_name)
{
    custom_audio_fmt_desired.freq = 48000;
    custom_audio_fmt_desired.format = AUDIO_S16;
    custom_audio_fmt_desired.channels = 2;
    custom_audio_fmt_desired.samples = 4096;
    custom_audio_fmt_desired.callback = mix_audio;
    custom_audio_fmt_desired.userdata = NULL;  
    SDL_AudioDeviceID device_id = SDL_OpenAudioDevice(device_name, 0, &custom_audio_fmt_desired, &custom_audio_fmt_obtained, 0);
    if (!device_id)
        printf("Error trying to initialize audio: %s\n", SDL_GetError());
    else
    {
        printf("Obtained audio device with:\n");
        printf("ID: %d\n", device_id);
        printf("FREQ: %d\n", custom_audio_fmt_obtained.freq);
        printf("CHANNELS: %d\n", custom_audio_fmt_obtained.channels);
        printf("SAMPLES: %d\n", custom_audio_fmt_obtained.samples);
        printf("FORMAT: ");

#define AUDIO_PRINT_CASE(x) case (x): printf(#x); break;
        
        switch (custom_audio_fmt_obtained.format)
        {
            AUDIO_PRINT_CASE(AUDIO_U8);
            AUDIO_PRINT_CASE(AUDIO_S8);
            AUDIO_PRINT_CASE(AUDIO_S16LSB);
            AUDIO_PRINT_CASE(AUDIO_S16MSB);
            AUDIO_PRINT_CASE(AUDIO_U16LSB);
            AUDIO_PRINT_CASE(AUDIO_U16MSB);
        }
        printf("\n");
    }
    flac_stream_decoder = flac_decoder_init();
    return device_id;
}


int main()
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO))
    {
        printf("Error: %s\n", SDL_GetError());
        return -1;
    }
    else
    {
        // GL 3.0 + GLSL 130
        const char* glsl_version = "#version 130";
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);


        // Create window with graphics context
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        SDL_Window* window = SDL_CreateWindow("Dear ImGui SDL2+OpenGL3 example", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, window_flags);
        SDL_GLContext gl_context = SDL_GL_CreateContext(window);
        SDL_GL_MakeCurrent(window, gl_context);
        SDL_GL_SetSwapInterval(1); // Enable vsync
        
        bool err = glewInit();
        if (err != GLEW_OK)
        {
            printf("Failed to initialize OpenGL loader\n");
        }
        else
        {
            printf("All good. Trying to init audio\n");

            

            // Setup Dear ImGui context
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO(); (void)io;
            //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
            //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

            // Setup Dear ImGui style
            ImGui::StyleColorsDark();
            //ImGui::StyleColorsClassic();

            // Setup Platform/Renderer backends
            ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
            ImGui_ImplOpenGL3_Init(glsl_version);


            // Init OpenGL texture to draw graphs to textures
            // TODO: Query for channel #
            if (!init_waveform_texture(LEFT_CHANNEL) || !init_waveform_texture(RIGHT_CHANNEL))
            {
                // TODO: Error handling
            }
            else
            {
                waveform_program = make_waveform_program();
                playing = false;
                bool done = false;
                bool sdl_mixer_initialized = false;
                bool sdl_custom_audio_initialized = false;
                typedef enum { NONE = 0, SDL_MIXER, SDL_CUSTOM_MIXER } mixer_enum_t;
                mixer_enum_t mixer = SDL_MIXER;
           
                while (!done)
                { // Main loop
                    SDL_Event event;
                    while (SDL_PollEvent(&event))
                    { // Event handling
                        ImGui_ImplSDL2_ProcessEvent(&event);
                        if (event.type == SDL_QUIT)
                            done = true;
                        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                            done = true;
                    } // Event handling end

                    // Start the Dear ImGui frame
                    ImGui_ImplOpenGL3_NewFrame();
                    ImGui_ImplSDL2_NewFrame(window);
                
                    ImGui::NewFrame();

                    // NOTE: Just testing
                    static bool texture_ready = false;
                    if (sdl_custom_audio_initialized && !texture_ready)
                    {
                        draw_waveform_to_texture(LEFT_CHANNEL);
                        draw_waveform_to_texture(RIGHT_CHANNEL);
                        texture_ready = true;
                    }

                    ImGui::Text("Press the button to play the song");
                    if (ImGui::SmallButton("Play"))
                    {
                        playing = !playing;
                    }

                    if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f, "%.4f"))
                    {                   
                        Mix_VolumeMusic(((Uint8)(volume * MIX_MAX_VOLUME)));
                    }
                    if (ImGui::RadioButton("Use SDL mixer", mixer == SDL_MIXER))
                    {
                        // TODO: Check if we are using the custom audio and close it
                        mixer = SDL_MIXER;
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Use SDL + custom mixer", mixer == SDL_CUSTOM_MIXER))
                    {
                        if (sdl_mixer_initialized)
                        {
                            if (playing)
                            {
                                playing = false;
                                Mix_HaltMusic();
                            }
                            Mix_Quit();
                            sdl_mixer_initialized = 0;
                        }
                        mixer = SDL_CUSTOM_MIXER;
                    }
                    ImGui::SameLine();
                    ImGui::Separator();

                    // TODO: Maybe this needs extra work for UTF-8 strings

                    static bool checked_audio_devices = false;
                    static char **audio_device_names;
                    static int audio_device_num;                

                    // TODO: Do some kind of check to see if the sound devices have changed!
                
                    if (!checked_audio_devices)
                    {
                        audio_device_num = SDL_GetNumAudioDevices(0);
                        audio_device_names = (char**) alloca(audio_device_num * sizeof(char**));

                        for (int i = 0; i < audio_device_num; ++i)
                        {
                            const char *orig_string = SDL_GetAudioDeviceName(i, 0);
                            int orig_str_len = strlen(orig_string) + 1;
                            audio_device_names[i] = (char*) alloca(orig_str_len);
                            strcpy(audio_device_names[i], orig_string);
                        }
                        checked_audio_devices = true;
                    }
                    else
                    {
                        if (ImGui::CollapsingHeader("Sound Devices"))
                        {
                            for (int i = 0; i < audio_device_num; ++i)
                            {
                                if (ImGui::RadioButton(audio_device_names[i], selected_audio_device_idx == i))
                                {
                                    selected_audio_device_idx = i;
                                }
                            }
                        }
                    }

               
                
                
                    if (mixer == SDL_MIXER)
                    {
                        if (!sdl_mixer_initialized)
                        {
                            if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048))
                            {
                                printf("Error trying to init audio\n");
                            }
                            else
                            {
                                printf("Mixer initialized. Trying to load sound\n");
                                if (!sdl_mixer_sound)
                                    sdl_mixer_sound = load_sound_from_file("sound/sound.flac");
                                sdl_mixer_initialized = true;
                            }
                        }
                    
                        if (playing)
                        {
                            if (!Mix_PlayingMusic())
                                Mix_PlayMusic(sdl_mixer_sound, -1);

                            if (Mix_PausedMusic())
                                Mix_ResumeMusic();

                            ImGui::Text("Playing");                    
                        }
                        else
                        {
                            if (Mix_PlayingMusic())
                            {
                                if (!Mix_PausedMusic())
                                    Mix_PauseMusic();                            
                            }
                        }         
                    }
                    else if (mixer == SDL_CUSTOM_MIXER)
                    {
                        if (song_ended)
                        {
                            song_ended = false;
                            playing = false;
                            SDL_PauseAudioDevice(custom_audio_open_device, 1);
                            restart_song();                        
                        }
                    
                        if (playing)
                        {
                            if (!sdl_custom_audio_initialized)
                            {
                                custom_audio_open_device = sdl_custom_audio_init(audio_device_names[selected_audio_device_idx]);
                                sdl_custom_audio_initialized = true;
                            }                                                    
                            SDL_PauseAudioDevice(custom_audio_open_device, 0);
                        }
                        else
                        {
                            SDL_PauseAudioDevice(custom_audio_open_device, 1);
                        }
                    }
                
                    ImGui::Text("mixer ");
                    if (mixer == SDL_CUSTOM_MIXER)
                        ImGui::Text("custom \n");
                    else
                        ImGui::Text("sdl mixer \n");
                
                    ImGui::Text("sdl_custom_audio_initialized %d\n", sdl_custom_audio_initialized);
                    ImGui::Text("sdl_mixer %d\n", sdl_mixer_initialized);
                    ImGui::Text("playing song %d\n", playing);

                    if (mixer == SDL_CUSTOM_MIXER && playing)
                    {
                        static bool wanna_rewind = false;
                        int total_samples = flac_client_data.total_samples;
                        int bytes_per_sample = flac_client_data.bits_per_sample/8;
                        int curr_sample = flac_client_data.play_position/bytes_per_sample;
                        static float progress;

                        if (!wanna_rewind)
                        {
                            progress = (float)curr_sample/(float)total_samples;                    
                        }
                        ImGui::Text("Curr sample %d\n", curr_sample);
                        ImGui::Text("Total samples %d\n", total_samples);
                    
                    
                        if (ImGui::SliderFloat("Progress\n", &progress, 0.0f, 1.0f))
                        {
                            wanna_rewind = true;
                        }

                        if (wanna_rewind && ImGui::IsMouseReleased(0))
                        {
                            // User manually rewinds the song
                            rewind_song(progress);
                            wanna_rewind = false;
                        }

                    }
                    ImGui::Image((ImTextureID)((intptr_t)waveform_texture_arr[LEFT_CHANNEL]), ImVec2(waveform_width, waveform_height));
                    ImGui::Image((ImTextureID)((intptr_t)waveform_texture_arr[RIGHT_CHANNEL]), ImVec2(waveform_width, waveform_height));

                    if (mixer == SDL_CUSTOM_MIXER && sdl_custom_audio_initialized)
                    {
                        if (ImGui::Button("Make spectrogram"))
                        {
                            if (spectrogram_data.padded_nsamples || init_spectrogram_data(&spectrogram_data, &flac_client_data))
                            {
                                printf("Nice!\n");
                                fft_rect(&spectrogram_data);
                            }
                        }
                    }
                    
                    // Rendering
                    

                
                    ImGui::Render();
                    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
                    glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
                    glClear(GL_COLOR_BUFFER_BIT);
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                    SDL_GL_SwapWindow(window);
                
                } // Main loop end
            }


            // Cleanup
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplSDL2_Shutdown();
            ImGui::DestroyContext();

            SDL_GL_DeleteContext(gl_context);
            SDL_DestroyWindow(window);
            SDL_Quit();
            
        }
    }

    return 0;
}
