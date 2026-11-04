/*
 * pylaunch.c - the python command, for the on-device build of the game code (runtime/builder.c runs it).
 * Android runs programs only from the app's native-library folder, which holds only lib*.so files: so this is
 * built as libpylaunch.so, an executable linked against the official Python for Android (libpython3.x.so, beside
 * it); the standard library comes from PYTHONHOME (extracted from the APK's assets).
 */
#include <Python.h>

int main(int argc, char **argv) {
    return Py_BytesMain(argc, argv);
}
