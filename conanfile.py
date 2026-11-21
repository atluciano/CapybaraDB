from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class CapybaraDBConan(ConanFile):
    name = "capybaradb"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("libpq/17.11")
        self.requires("openssl/3.6.5")
        self.requires("freetype/2.13.2")
        self.requires("harfbuzz/11.4.1")
        self.requires("nlohmann_json/3.12.0")
        self.requires("sqlite3/3.54.1")
        self.requires("catch2/3.16.1")
        self.requires("nanobench/4.3.11")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.user_presets_path = False
        tc.generate()
