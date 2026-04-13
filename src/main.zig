// The shared object is built by CMake from vendor/baresip.
// This module exists so the wrapper has a standard Zig entry point.
comptime {
    _ = @import("std");
}
