const std = @import("std");
const build_utils = @import("build_utils.zig");

const TargetParts = struct {
    arch: []const u8,
    libc: []const u8,
};

const ReBuild = struct {
    install_step: *std.Build.Step,
    install_dir: []const u8,
};

const MbedtlsBuild = struct {
    install_step: *std.Build.Step,
    install_dir: []const u8,
};

fn getCmakeBuildType(optimize: std.builtin.OptimizeMode) []const u8 {
    return switch (optimize) {
        .Debug => "Debug",
        .ReleaseSafe => "RelWithDebInfo",
        .ReleaseFast => "Release",
        .ReleaseSmall => "MinSizeRel",
    };
}

fn getTargetParts(target: std.Build.ResolvedTarget) TargetParts {
    if (target.result.os.tag != .linux) {
        std.debug.panic("baresip wrapper supports linux targets only, got {s}", .{
            @tagName(target.result.os.tag),
        });
    }

    const arch = switch (target.result.cpu.arch) {
        .x86_64 => "x86_64",
        .aarch64 => "aarch64",
        else => std.debug.panic("unsupported cpu arch for baresip: {s}", .{
            @tagName(target.result.cpu.arch),
        }),
    };

    const libc = switch (target.result.abi) {
        .gnu, .gnueabi, .gnueabihf => "gnu",
        .musl, .musleabi, .musleabihf => "musl",
        else => std.debug.panic("unsupported abi for baresip: {s}", .{
            @tagName(target.result.abi),
        }),
    };

    return .{
        .arch = arch,
        .libc = libc,
    };
}

fn addMbedtlsBuild(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) MbedtlsBuild {
    const target_str = build_utils.getTargetString(target);
    const cmake_build_type = getCmakeBuildType(optimize);
    const target_parts = getTargetParts(target);
    const target_triple = b.fmt("{s}-linux-{s}", .{ target_parts.arch, target_parts.libc });

    const cmake_build_dir = b.fmt(".zig-cache/mbedtls-dep/{s}/{s}", .{ target_str, cmake_build_type });
    const cmake_install_dir = b.fmt("{s}/install", .{cmake_build_dir});
    const cmake_install_dir_abs = b.pathFromRoot(cmake_install_dir);

    const configure = b.addSystemCommand(&[_][]const u8{
        "cmake",
        "-S",
        "../mbedtls/vendor/mbedtls",
        "-B",
        cmake_build_dir,
        "-G",
        "Ninja",
        b.fmt("-DCMAKE_BUILD_TYPE={s}", .{cmake_build_type}),
        "-DCMAKE_SYSTEM_NAME=Linux",
        b.fmt("-DCMAKE_SYSTEM_PROCESSOR={s}", .{target_parts.arch}),
        "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
        "-DCMAKE_C_COMPILER=zig",
        "-DCMAKE_C_COMPILER_ARG1=cc",
        b.fmt("-DCMAKE_C_COMPILER_TARGET={s}", .{target_triple}),
        "-DCMAKE_C_FLAGS=-DMBEDTLS_SSL_DTLS_SRTP",
        "-DENABLE_TESTING=OFF",
        "-DENABLE_PROGRAMS=OFF",
        "-DGEN_FILES=OFF",
        "-DMBEDTLS_FATAL_WARNINGS=OFF",
        "-DUSE_SHARED_MBEDTLS_LIBRARY=ON",
        "-DUSE_STATIC_MBEDTLS_LIBRARY=OFF",
    });
    configure.setName(b.fmt("configure mbedtls dep ({s})", .{target_str}));

    const build_cmd = b.addSystemCommand(&[_][]const u8{
        "cmake",
        "--build",
        cmake_build_dir,
        "--config",
        cmake_build_type,
        "--target",
        "lib",
        "--parallel",
    });
    build_cmd.setName(b.fmt("build mbedtls dep ({s})", .{target_str}));
    build_cmd.step.dependOn(&configure.step);

    const install_cmd = b.addSystemCommand(&[_][]const u8{
        "cmake",
        "--install",
        cmake_build_dir,
        "--prefix",
        cmake_install_dir_abs,
    });
    install_cmd.setName(b.fmt("install mbedtls dep ({s})", .{target_str}));
    install_cmd.step.dependOn(&build_cmd.step);

    return .{
        .install_step = &install_cmd.step,
        .install_dir = cmake_install_dir_abs,
    };
}

fn addReStaticBuild(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    mbedtls_build: MbedtlsBuild,
) ReBuild {
    const target_str = build_utils.getTargetString(target);
    const cmake_build_type = getCmakeBuildType(optimize);
    const target_parts = getTargetParts(target);
    const target_triple = b.fmt("{s}-linux-{s}", .{ target_parts.arch, target_parts.libc });

    const re_build_dir = b.fmt(".zig-cache/baresip/re/{s}/{s}", .{ target_str, cmake_build_type });
    const re_install_dir = b.fmt("{s}/install", .{re_build_dir});
    const mbedtls_prefix = mbedtls_build.install_dir;
    const mbedtls_include_dir = b.fmt("{s}/include", .{mbedtls_prefix});
    const mbedtls_lib_dir = b.fmt("{s}/lib", .{mbedtls_prefix});
    const mbedtls_lib = b.fmt("{s}/libmbedtls.so", .{mbedtls_lib_dir});

    const configure = b.addSystemCommand(&[_][]const u8{
        "cmake",
        "-S",
        "vendor/re",
        "-B",
        re_build_dir,
        "-G",
        "Ninja",
        b.fmt("-DCMAKE_BUILD_TYPE={s}", .{cmake_build_type}),
        "-DCMAKE_SYSTEM_NAME=Linux",
        b.fmt("-DCMAKE_SYSTEM_PROCESSOR={s}", .{target_parts.arch}),
        "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
        "-DCMAKE_C_COMPILER=zig",
        "-DCMAKE_C_COMPILER_ARG1=cc",
        b.fmt("-DCMAKE_C_COMPILER_TARGET={s}", .{target_triple}),
        "-DCMAKE_C_FLAGS=-DMBEDTLS_SSL_DTLS_SRTP",
        "-DLIBRE_BUILD_SHARED=OFF",
        "-DLIBRE_BUILD_STATIC=ON",
        "-DUSE_MBEDTLS=ON",
        "-DUSE_OPENSSL=OFF",
        "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON",
        "-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON",
        "-DCMAKE_DISABLE_FIND_PACKAGE_Backtrace=ON",
        b.fmt("-DOPENSSL_INCLUDE_DIR={s}", .{mbedtls_include_dir}),
        "-DZLIB_INCLUDE_DIRS=",
        b.fmt("-DMBEDTLS_INCLUDE_DIRS={s}", .{mbedtls_include_dir}),
        b.fmt("-DMBEDTLS_LIBRARY_DIRS={s}", .{mbedtls_lib_dir}),
        b.fmt("-DMBEDTLS_HINTS={s}", .{mbedtls_prefix}),
        b.fmt("-DMBEDTLS_LIBRARY={s}", .{mbedtls_lib}),
    });
    configure.setName(b.fmt("configure re ({s})", .{target_str}));
    configure.step.dependOn(mbedtls_build.install_step);

    const build_cmd = b.addSystemCommand(&[_][]const u8{
        "cmake",
        "--build",
        re_build_dir,
        "--config",
        cmake_build_type,
        "--target",
        "re",
        "--parallel",
    });
    build_cmd.setName(b.fmt("build re ({s})", .{target_str}));
    build_cmd.step.dependOn(&configure.step);

    const install_cmd = b.addSystemCommand(&[_][]const u8{
        "cmake",
        "--install",
        re_build_dir,
        "--prefix",
        re_install_dir,
    });
    install_cmd.setName(b.fmt("install re ({s})", .{target_str}));
    install_cmd.step.dependOn(&build_cmd.step);

    return .{
        .install_step = &install_cmd.step,
        .install_dir = re_install_dir,
    };
}

fn addBaresipSharedBuild(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    lib_name: []const u8,
) *std.Build.Step.InstallFile {
    const target_str = build_utils.getTargetString(target);
    const cmake_build_type = getCmakeBuildType(optimize);
    const target_parts = getTargetParts(target);
    const target_triple = b.fmt("{s}-linux-{s}", .{ target_parts.arch, target_parts.libc });

    const mbedtls_build = addMbedtlsBuild(b, target, optimize);
    const re_build = addReStaticBuild(b, target, optimize, mbedtls_build);
    const mbedtls_prefix = mbedtls_build.install_dir;
    const mbedtls_include_dir = b.fmt("{s}/include", .{mbedtls_prefix});
    const mbedtls_lib_dir = b.fmt("{s}/lib", .{mbedtls_prefix});
    const mbedtls_lib = b.fmt("{s}/libmbedtls.so", .{mbedtls_lib_dir});
    const mbedtls_linker_flags = b.fmt(
        "-DCMAKE_SHARED_LINKER_FLAGS=-L{s} -lmbedtls -lmbedx509 -lmbedcrypto",
        .{mbedtls_lib_dir},
    );

    const baresip_build_dir = b.fmt(".zig-cache/baresip/baresip/{s}/{s}", .{ target_str, cmake_build_type });
    const re_library = b.fmt("{s}/lib/libre.a", .{re_build.install_dir});
    const re_include = b.fmt("{s}/include/re", .{re_build.install_dir});

    const configure = b.addSystemCommand(&[_][]const u8{
        "cmake",
        "-S",
        "vendor/baresip",
        "-B",
        baresip_build_dir,
        "-G",
        "Ninja",
        b.fmt("-DCMAKE_BUILD_TYPE={s}", .{cmake_build_type}),
        "-DCMAKE_SYSTEM_NAME=Linux",
        b.fmt("-DCMAKE_SYSTEM_PROCESSOR={s}", .{target_parts.arch}),
        "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
        "-DCMAKE_C_COMPILER=zig",
        "-DCMAKE_C_COMPILER_ARG1=cc",
        "-DCMAKE_CXX_COMPILER=zig",
        "-DCMAKE_CXX_COMPILER_ARG1=c++",
        b.fmt("-DCMAKE_C_COMPILER_TARGET={s}", .{target_triple}),
        b.fmt("-DCMAKE_CXX_COMPILER_TARGET={s}", .{target_triple}),
        "-DCMAKE_C_FLAGS=-DMBEDTLS_SSL_DTLS_SRTP",
        "-DCMAKE_CXX_FLAGS=-DMBEDTLS_SSL_DTLS_SRTP",
        "-DMODULES=",
        b.fmt("-DRE_LIBRARY={s}", .{re_library}),
        b.fmt("-DRE_INCLUDE_DIR={s}", .{re_include}),
        "-DUSE_MBEDTLS=ON",
        "-DUSE_OPENSSL=OFF",
        "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON",
        "-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON",
        "-DCMAKE_DISABLE_FIND_PACKAGE_Backtrace=ON",
        b.fmt("-DCMAKE_PREFIX_PATH={s}", .{mbedtls_prefix}),
        b.fmt("-DMBEDTLS_INCLUDE_DIRS={s}", .{mbedtls_include_dir}),
        b.fmt("-DMBEDTLS_LIBRARY_DIRS={s}", .{mbedtls_lib_dir}),
        b.fmt("-DMBEDTLS_HINTS={s}", .{mbedtls_prefix}),
        b.fmt("-DMBEDTLS_LIBRARY={s}", .{mbedtls_lib}),
        mbedtls_linker_flags,
    });
    configure.setName(b.fmt("configure baresip ({s})", .{target_str}));
    configure.step.dependOn(re_build.install_step);

    const build_cmd = b.addSystemCommand(&[_][]const u8{
        "cmake",
        "--build",
        baresip_build_dir,
        "--config",
        cmake_build_type,
        "--target",
        "libbaresip.so",
        "--parallel",
    });
    build_cmd.setName(b.fmt("build libbaresip ({s})", .{target_str}));
    build_cmd.step.dependOn(&configure.step);

    const built_library = b.fmt("{s}/libbaresip.so", .{baresip_build_dir});
    const installed_library_name = b.fmt("lib{s}.so", .{lib_name});
    const install_lib = b.addInstallFileWithDir(
        .{ .cwd_relative = built_library },
        .lib,
        installed_library_name,
    );
    install_lib.step.dependOn(&build_cmd.step);

    return install_lib;
}

fn buildForTarget(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    artifacts_dir: []const u8,
    hashes: *std.StringHashMap([]const u8),
    json_step: *build_utils.WriteJsonStep,
) void {
    const target_str = build_utils.getTargetString(target);
    const lib_name = build_utils.getLibName(std.heap.page_allocator, "baresip", target_str);
    const install_lib = addBaresipSharedBuild(b, target, optimize, lib_name);

    const hash_step = build_utils.HashAndMoveStep.create(
        b,
        lib_name,
        target_str,
        artifacts_dir,
        hashes,
    );
    hash_step.step.dependOn(&install_lib.step);

    json_step.step.dependOn(&hash_step.step);
}

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{});
    const artifacts_dir = "../../artifacts/libs";
    const json_path = "current.json";

    const build_all = b.option(bool, "all", "Build for all supported targets") orelse false;

    if (build_all) {
        const hashes = build_utils.createHashMap(b);
        const json_step = build_utils.WriteJsonStep.create(b, hashes, json_path);

        for (build_utils.supported_targets) |query| {
            const target = b.resolveTargetQuery(query);
            buildForTarget(b, target, optimize, artifacts_dir, hashes, json_step);
        }

        b.default_step.dependOn(&json_step.step);
    } else {
        const target = b.standardTargetOptions(.{});
        const install_lib = addBaresipSharedBuild(b, target, optimize, "baresip");
        const install_header = b.addInstallHeaderFile(
            b.path("vendor/baresip/include/baresip.h"),
            "baresip/baresip.h",
        );

        b.getInstallStep().dependOn(&install_lib.step);
        b.getInstallStep().dependOn(&install_header.step);
    }
}
