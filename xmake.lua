add_rules("mode.release", "mode.debug")
set_policy("package.requires_lock", false)

package("preloader")
    set_homepage("https://github.com/LiteLDev/preloader-android")
    set_description("Preloader Android")
    add_urls("https://github.com/LiteLDev/preloader-android.git")
    add_versions("main", "main")
    add_deps("cmake")
    on_install("android", function (package)
        import("package.tools.cmake").install(package)
    end)
package_end()

add_requires("preloader")
add_requires("fmt")

target("Optimization")
    set_kind("shared")
    set_languages("c++20")
    set_strip("all")
    add_files("src/*.cpp")
    add_includedirs("include", {public = true})
    add_packages("preloader", "fmt")
    if is_plat("android") then
        add_cxflags("-fPIC", "-Oz", "-ffunction-sections", "-fdata-sections", "-fno-rtti", "-fexceptions", "-w")
        add_shflags("-Wl,--gc-sections", "-Wl,-z,max-page-size=16384")
        add_links("android", "log", "EGL")
    end
    after_build(function (target)
        if not target:is_plat("android") then return end
        import("lib.detect.find_tool")
        local python = find_tool("python3") or find_tool("python")
        assert(python, "python3 required")
        os.execv(python.program, {
            path.join(os.projectdir(), "scripts", "package_levipack.py"),
            "--library", target:targetfile(),
            "--icon", path.join(os.projectdir(), "assets", "icon.png"),
            "--output", path.join(target:targetdir(), "Optimization.levipack"),
        })
    end)
