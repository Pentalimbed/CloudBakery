-- set minimum xmake version
set_xmakever("2.8.2")

-- set project
set_project("cloud-bakery")
set_version("0.0.0")

-- set defaults
set_languages("c++23")
set_warnings("allextra")

-- set policies
set_policy("package.requires_lock", true)

-- add rules
add_rules("mode.debug", "mode.releasedbg")

-- add requires
add_requires("argparse", "spdlog", "re2", "directxmath", "directxtex", "directxtk")

-- targets
target("cloud-bakery")
    add_packages("argparse", "spdlog", "re2", "directxmath", "directxtex", "directxtk")
    add_syslinks("dxgi", "d3dcompiler", "d3d11", "user32")

    add_files("src/**.cpp")
    -- add_headerfiles("src/**.h")
    add_includedirs("src")