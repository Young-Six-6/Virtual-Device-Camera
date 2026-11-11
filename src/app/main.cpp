#include <Windows.h>
#include <stdio.h>
#include <string.h>

#include "../camera/uvc_vcam.h"
#include "../media/frame_source.h"

static bool set_executable_directory()
{
    char executable_path[MAX_PATH + 1] = {};
    const DWORD path_length = GetModuleFileNameA(NULL, executable_path, MAX_PATH);
    if (path_length == 0 || path_length >= MAX_PATH)
    {
        fprintf(stderr, "Unable to read the executable path (error %lu).\n", GetLastError());
        return false;
    }

    char* separator = strrchr(executable_path, '\\');
    if (!separator)
    {
        fprintf(stderr, "Unable to determine the executable directory.\n");
        return false;
    }

    *separator = '\0';
    if (!SetCurrentDirectoryA(executable_path))
    {
        fprintf(stderr, "Unable to set the executable directory as working directory (error %lu).\n",
            GetLastError());
        return false;
    }
    return true;
}

static void configure_camera(uvc_vcam_t* camera, int pid, int vid, const char* product,
    frame_source* source)
{
    camera->pid = pid;
    camera->vid = vid;
    camera->manu_fact = "Fanxiushu";
    camera->product = product;
    camera->frame_callback = frame_source_callback;
    camera->param = source;
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if (!set_executable_directory()) return 1;

    if (argc > 3)
    {
        fprintf(stderr, "Usage: %s [video1.mp4 [video2.mp4]]\n", argv[0]);
        return 2;
    }

    const bool pipe_mode = argc == 1;
    frame_source* source1 = pipe_mode
        ? frame_source_create_pipe()
        : frame_source_create_file(argv[1]);
    frame_source* source2 = argc == 3 ? frame_source_create_file(argv[2]) : NULL;
    if (!source1 || (argc == 3 && !source2))
    {
        fprintf(stderr, "Unable to allocate a video frame source.\n");
        frame_source_destroy(source2);
        frame_source_destroy(source1);
        return 1;
    }

    if (pipe_mode)
    {
        printf("Camera 1 is waiting for RGB24 input on \\\\.\\pipe\\vcam_pipe.\n");
        printf("Usage: %s <video1.mp4> [video2.mp4]\n", argv[0]);
    }
    else
    {
        printf("Camera 1 input: %s\n", argv[1]);
        if (argc == 3) printf("Camera 2 input: %s\n", argv[2]);
    }

    if (pipe_mode && !frame_source_start_pipe_listener(source1))
        fprintf(stderr, "Warning: camera 1 will remain black because the named pipe could not be created.\n");

    uvc_vcam_t camera1 = {};
    configure_camera(&camera1, 0xcc10, 0xbb10,
        pipe_mode ? "Pipe HD Camera 1" : "MP4 HD Camera 1", source1);

    void* camera1_handle = vcam_create(&camera1);
    if (!camera1_handle)
    {
        fprintf(stderr, "Unable to create camera 1; a compatible virtual USB bus driver is required.\n");
        frame_source_destroy(source2);
        frame_source_destroy(source1);
        return 1;
    }

    void* camera2_handle = NULL;
    if (source2)
    {
        uvc_vcam_t camera2 = {};
        configure_camera(&camera2, 0xcc11, 0xbb11, "MP4 HD Camera 2", source2);
        camera2_handle = vcam_create(&camera2);
        if (!camera2_handle)
            fprintf(stderr, "Warning: unable to create camera 2.\n");
    }

    printf("\nPress Enter to exit.\n");
    (void)getchar();

    if (camera2_handle) vcam_destroy(camera2_handle);
    vcam_destroy(camera1_handle);
    frame_source_destroy(source2);
    frame_source_destroy(source1);

    printf("Application exited.\n");
    return 0;
}
