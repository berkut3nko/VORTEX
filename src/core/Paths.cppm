module;

#include <filesystem>
#include <string_view>

export module vortex.core:paths;

export namespace vortex {

    /**
     * @brief Locations the engine reads from / writes to.
     * @details Assets are resolved relative to the executable, never the working directory,
     * so the game starts correctly from a shell, a file manager or an IDE.
     */
    class Paths {
    public:
        /// @brief Directory containing the running executable.
        static const std::filesystem::path& ExecutableDir();
        /// @brief Root of read-only assets. Defaults to `<exe dir>/assets`.
        static const std::filesystem::path& AssetRoot();
        static void SetAssetRoot(const std::filesystem::path& root);
        /// @brief `AssetRoot() / relative`.
        static std::filesystem::path Asset(std::string_view relative);
    };
}
