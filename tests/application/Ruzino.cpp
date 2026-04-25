#define _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/stringUtils.h>
#include <rzconsole/ConsoleInterpreter.h>
#include <rzconsole/ConsoleObjects.h>
#include <rzconsole/imgui_console.h>
#include <rzconsole/spdlog_console_sink.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <any>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <rzpython/interpreter.hpp>
#include <rzpython/rzpython.hpp>

#include "GCore/algorithms/intersection.h"
#include "GCore/geom_payload.hpp"
#include "GUI/ImGuiFileDialog.h"
#include "GUI/window.h"
#include "MCore/MaterialXNodeTree.hpp"
#include "MCore/MaterialXNodeTreeWidget.h"
#include "nodes/core/io/json.hpp"
#include "nodes/system/node_system.hpp"
#include "nodes/ui/imgui.hpp"
#include "pxr/base/tf/setenv.h"
#include "pxr/usd/usd/stage.h"
#include "stage/stage.hpp"
#include "usd_nodejson.hpp"
#include "widgets/usdtree/usd_fileviewer.h"
#include "widgets/usdview/usdview_widget.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

using namespace Ruzino;
namespace mx = MaterialX;

namespace {
constexpr std::size_t kTerminalLogMaxSize = 8 * 1024 * 1024;
constexpr int kTerminalLogMaxFiles = 3;
constexpr std::size_t kTerminalStreamBufferSize = 4 * 1024;

enum class TerminalCaptureMode {
    Disabled,
    Stderr,
    StdoutStderr,
};

enum class TerminalRetentionMode {
    Overwrite,
    Append,
    Rotate,
};

enum class TerminalFlushMode {
    Balanced,
    Realtime,
    Performance,
};

struct TerminalCaptureConfig {
    TerminalCaptureMode mode = TerminalCaptureMode::Stderr;
    TerminalRetentionMode retention = TerminalRetentionMode::Overwrite;
    TerminalFlushMode flush = TerminalFlushMode::Balanced;
};

struct RendererNodeWidgetSettings : public FileBasedNodeWidgetSettings {
    std::string widget_name;

    std::string WidgetName() const override
    {
        return widget_name;
    }
};

class UsdDiagnosticFileDelegate final : public pxr::TfDiagnosticMgr::Delegate {
   public:
    void IssueError(const pxr::TfError& err) override
    {
        LogDiagnostic("USD error", err, spdlog::level::err);
    }

    void IssueFatalError(
        const pxr::TfCallContext& context,
        const std::string& msg) override
    {
        spdlog::critical(
            "USD fatal [{}:{}] {}: {}",
            context.GetFile(),
            context.GetLine(),
            context.GetFunction(),
            msg);
        if (auto logger = spdlog::default_logger()) {
            logger->flush();
        }
        _UnhandledAbort();
    }

    void IssueStatus(const pxr::TfStatus& status) override
    {
        LogDiagnostic("USD status", status, spdlog::level::info);
    }

    void IssueWarning(const pxr::TfWarning& warning) override
    {
        LogDiagnostic("USD warning", warning, spdlog::level::warn);
    }

   private:
    template<typename DiagnosticT>
    static void LogDiagnostic(
        const char* kind,
        const DiagnosticT& diagnostic,
        spdlog::level::level_enum level)
    {
        spdlog::log(
            level,
            "{} [{}] [{}:{}] {}: {}",
            kind,
            diagnostic.GetDiagnosticCodeAsString(),
            diagnostic.GetSourceFileName(),
            diagnostic.GetSourceLineNumber(),
            diagnostic.GetSourceFunction(),
            diagnostic.GetCommentary());
        if (auto logger = spdlog::default_logger()) {
            logger->flush();
        }
    }
};

const char* to_string(TerminalCaptureMode mode)
{
    switch (mode) {
        case TerminalCaptureMode::Disabled: return "disabled";
        case TerminalCaptureMode::Stderr: return "stderr";
        case TerminalCaptureMode::StdoutStderr: return "stdout_stderr";
    }
    return "stderr";
}

const char* to_string(TerminalRetentionMode retention)
{
    switch (retention) {
        case TerminalRetentionMode::Overwrite: return "overwrite";
        case TerminalRetentionMode::Append: return "append";
        case TerminalRetentionMode::Rotate: return "rotate";
    }
    return "overwrite";
}

const char* to_string(TerminalFlushMode flush)
{
    switch (flush) {
        case TerminalFlushMode::Balanced: return "balanced";
        case TerminalFlushMode::Realtime: return "realtime";
        case TerminalFlushMode::Performance: return "performance";
    }
    return "balanced";
}

std::filesystem::path get_terminal_capture_config_path()
{
    return std::filesystem::current_path() / "Ruzino.logging.json";
}

template<typename EnumT>
bool parse_config_enum(
    const nlohmann::json& value,
    EnumT& out_value,
    std::initializer_list<std::pair<const char*, EnumT>> mapping)
{
    if (!value.is_string()) {
        return false;
    }

    const std::string requested = value.get<std::string>();
    for (const auto& [key, enum_value] : mapping) {
        if (requested == key) {
            out_value = enum_value;
            return true;
        }
    }
    return false;
}

TerminalCaptureConfig load_terminal_capture_config()
{
    TerminalCaptureConfig config;
    const auto config_path = get_terminal_capture_config_path();
    if (!std::filesystem::exists(config_path)) {
        spdlog::info(
            "Terminal capture config not found at {}; using defaults: mode={}, retention={}, flush={}",
            config_path.string(),
            to_string(config.mode),
            to_string(config.retention),
            to_string(config.flush));
        return config;
    }

    try {
        std::ifstream file(config_path);
        nlohmann::json json;
        file >> json;

        if (!json.is_object()) {
            throw std::runtime_error("root must be a JSON object");
        }

        if (json.contains("terminal_capture")) {
            const auto& capture_json = json.at("terminal_capture");
            if (!capture_json.is_object()) {
                throw std::runtime_error("'terminal_capture' must be an object");
            }

            if (capture_json.contains("mode") &&
                !parse_config_enum(
                    capture_json.at("mode"),
                    config.mode,
                    { { "disabled", TerminalCaptureMode::Disabled },
                      { "stderr", TerminalCaptureMode::Stderr },
                      { "stdout_stderr", TerminalCaptureMode::StdoutStderr } })) {
                spdlog::warn(
                    "Invalid terminal_capture.mode in {}; using default '{}'",
                    config_path.string(),
                    to_string(config.mode));
            }

            if (capture_json.contains("retention") &&
                !parse_config_enum(
                    capture_json.at("retention"),
                    config.retention,
                    { { "overwrite", TerminalRetentionMode::Overwrite },
                      { "append", TerminalRetentionMode::Append },
                      { "rotate", TerminalRetentionMode::Rotate } })) {
                spdlog::warn(
                    "Invalid terminal_capture.retention in {}; using default '{}'",
                    config_path.string(),
                    to_string(config.retention));
            }

            if (capture_json.contains("flush") &&
                !parse_config_enum(
                    capture_json.at("flush"),
                    config.flush,
                    { { "balanced", TerminalFlushMode::Balanced },
                      { "realtime", TerminalFlushMode::Realtime },
                      { "performance", TerminalFlushMode::Performance } })) {
                spdlog::warn(
                    "Invalid terminal_capture.flush in {}; using default '{}'",
                    config_path.string(),
                    to_string(config.flush));
            }
        }

        spdlog::info(
            "Loaded terminal capture config from {}: mode={}, retention={}, flush={}",
            config_path.string(),
            to_string(config.mode),
            to_string(config.retention),
            to_string(config.flush));
    }
    catch (const std::exception& e) {
        config = TerminalCaptureConfig{};
        spdlog::warn(
            "Failed to parse {}; using defaults: {}",
            config_path.string(),
            e.what());
    }

    return config;
}

#ifdef _WIN32
TerminalCaptureConfig g_terminal_capture_config;
bool g_stdout_captured = false;
bool g_stderr_captured = false;
const std::filesystem::path g_stdout_capture_path = "logs/Ruzino.stdout.log";
const std::filesystem::path g_stderr_capture_path = "logs/Ruzino.stderr.log";
#endif
std::unique_ptr<UsdDiagnosticFileDelegate> g_usd_diagnostic_delegate;

#ifdef _WIN32
void rotate_terminal_file(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto oldest =
        std::filesystem::path(path.string() + "." +
                              std::to_string(kTerminalLogMaxFiles));
    std::filesystem::remove(oldest, ec);

    for (int i = kTerminalLogMaxFiles - 1; i >= 1; --i) {
        const auto src =
            std::filesystem::path(path.string() + "." + std::to_string(i));
        const auto dst =
            std::filesystem::path(path.string() + "." + std::to_string(i + 1));
        if (std::filesystem::exists(src, ec)) {
            std::filesystem::remove(dst, ec);
            std::filesystem::rename(src, dst, ec);
        }
    }

    if (std::filesystem::exists(path, ec)) {
        const auto rotated = std::filesystem::path(path.string() + ".1");
        std::filesystem::remove(rotated, ec);
        std::filesystem::rename(path, rotated, ec);
    }
}

void prepare_terminal_log_target(
    const std::filesystem::path& path,
    TerminalRetentionMode retention)
{
    std::filesystem::create_directories(path.parent_path());

    if (retention != TerminalRetentionMode::Rotate) {
        return;
    }

    std::error_code ec;
    if (std::filesystem::exists(path, ec) &&
        std::filesystem::file_size(path, ec) >= kTerminalLogMaxSize) {
        rotate_terminal_file(path);
    }
}

void update_standard_handle(FILE* stream)
{
    const intptr_t os_handle = _get_osfhandle(_fileno(stream));
    if (os_handle == -1) {
        return;
    }

    if (stream == stdout) {
        SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(os_handle));
    }
    else if (stream == stderr) {
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(os_handle));
    }
}

bool capture_terminal_stream(
    FILE* stream,
    const std::filesystem::path& path,
    TerminalRetentionMode retention,
    TerminalFlushMode flush)
{
    prepare_terminal_log_target(path, retention);

    const char* mode = retention == TerminalRetentionMode::Append ? "a" : "w";
    FILE* reopened = nullptr;
    if (freopen_s(&reopened, path.string().c_str(), mode, stream) != 0 ||
        reopened == nullptr) {
        return false;
    }

    const int buffer_mode =
        flush == TerminalFlushMode::Performance ? _IOFBF : _IOLBF;
    setvbuf(stream, nullptr, buffer_mode, kTerminalStreamBufferSize);
    update_standard_handle(stream);
    return true;
}

void maybe_rotate_captured_stream(
    FILE* stream,
    bool enabled,
    const std::filesystem::path& path)
{
    if (!enabled || g_terminal_capture_config.retention !=
                        TerminalRetentionMode::Rotate) {
        return;
    }

    std::fflush(stream);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) ||
        std::filesystem::file_size(path, ec) < kTerminalLogMaxSize) {
        return;
    }

    rotate_terminal_file(path);
    FILE* reopened = nullptr;
    if (freopen_s(&reopened, path.string().c_str(), "w", stream) == 0 &&
        reopened != nullptr) {
        const int buffer_mode =
            g_terminal_capture_config.flush == TerminalFlushMode::Performance
            ? _IOFBF
            : _IOLBF;
        setvbuf(stream, nullptr, buffer_mode, kTerminalStreamBufferSize);
        update_standard_handle(stream);
    }
}
#endif

void flush_terminal_streams()
{
    std::fflush(stdout);
    std::fflush(stderr);

#ifdef _WIN32
    maybe_rotate_captured_stream(stdout, g_stdout_captured, g_stdout_capture_path);
    maybe_rotate_captured_stream(stderr, g_stderr_captured, g_stderr_capture_path);
#endif
}

void configure_application_logging()
{
    namespace fs = std::filesystem;

    fs::create_directories("logs");

#ifdef _DEBUG
    constexpr auto console_level = spdlog::level::debug;
#else
    constexpr auto console_level = spdlog::level::warn;
#endif

    std::vector<spdlog::sink_ptr> sinks;
    auto previous_default_logger = spdlog::default_logger();
    if (previous_default_logger) {
        sinks = previous_default_logger->sinks();
    }

    if (sinks.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    }

    for (auto& sink : sinks) {
        if (!sink) {
            continue;
        }
        sink->set_level(console_level);
        sink->set_pattern("%^[%T] %n: %v%$");
    }

    auto full_log_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        "logs/Ruzino.log", true);
    full_log_sink->set_level(spdlog::level::trace);
    full_log_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    sinks.push_back(full_log_sink);

    auto error_log_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        "logs/Ruzino.error.log", true);
    error_log_sink->set_level(spdlog::level::warn);
    error_log_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    sinks.push_back(error_log_sink);

    auto logger =
        std::make_shared<spdlog::logger>("Ruzino", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::info);

    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(1));
    spdlog::info(
        "Auto-save logging enabled: logs/Ruzino.log and logs/Ruzino.error.log");
}

void install_usd_diagnostic_logging()
{
    if (!g_usd_diagnostic_delegate) {
        g_usd_diagnostic_delegate = std::make_unique<UsdDiagnosticFileDelegate>();
        pxr::TfDiagnosticMgr::GetInstance().AddDelegate(
            g_usd_diagnostic_delegate.get());
        spdlog::info("Installed OpenUSD diagnostic file delegate");
    }
}

void uninstall_usd_diagnostic_logging()
{
    if (g_usd_diagnostic_delegate) {
        pxr::TfDiagnosticMgr::GetInstance().RemoveDelegate(
            g_usd_diagnostic_delegate.get());
        g_usd_diagnostic_delegate.reset();
    }
}

void configure_terminal_capture(const TerminalCaptureConfig& config)
{
#ifdef _WIN32
    g_terminal_capture_config = config;
    g_stdout_captured = false;
    g_stderr_captured = false;

    if (config.mode == TerminalCaptureMode::Disabled) {
        spdlog::info(
            "Terminal capture disabled by config: mode={}, retention={}, flush={}",
            to_string(config.mode),
            to_string(config.retention),
            to_string(config.flush));
        return;
    }

    if (config.mode == TerminalCaptureMode::Stderr ||
        config.mode == TerminalCaptureMode::StdoutStderr) {
        g_stderr_captured = capture_terminal_stream(
            stderr, g_stderr_capture_path, config.retention, config.flush);
        if (!g_stderr_captured) {
            spdlog::warn(
                "Failed to redirect stderr to {}",
                g_stderr_capture_path.string());
        }
    }

    if (config.mode == TerminalCaptureMode::StdoutStderr) {
        g_stdout_captured = capture_terminal_stream(
            stdout, g_stdout_capture_path, config.retention, config.flush);
        if (!g_stdout_captured) {
            spdlog::warn(
                "Failed to redirect stdout to {}",
                g_stdout_capture_path.string());
        }
    }

    spdlog::info(
        "Terminal capture enabled by config: mode={}, retention={}, flush={}",
        to_string(config.mode),
        to_string(config.retention),
        to_string(config.flush));
#else
    (void)config;
    spdlog::warn("Terminal capture is only implemented on Windows");
#endif
}
}  // namespace

class MaterialXNodeSystem : public NodeSystem {
   public:
    MaterialXNodeSystem()
    {
        descriptor = std::make_shared<MaterialXNodeTreeDescriptor>();
    }

    // Factory method to create a MaterialX system with a default material
    // document
    static std::shared_ptr<MaterialXNodeSystem> create_with_default_material(
        const std::string& material_name,
        mx::DocumentPtr ptr = nullptr)
    {
        auto system = std::make_shared<MaterialXNodeSystem>();

        mx::DocumentPtr doc;
        // Create a minimal MaterialX document in memory using MaterialX API
        if (!ptr) {
            doc = mx::createDocument();

            // CRITICAL FIX: Create complete material with standard_surface
            // shader This ensures the .mtlx file has children BEFORE the
            // reference is created
            mx::NodePtr materialNode =
                doc->addNode("surfacematerial", material_name, "material");

            // Create standard_surface shader node with proper nodedef
            std::string shader_name = "standard_surface_surfaceshader";
            mx::NodePtr shaderNode =
                doc->addNode("standard_surface", shader_name, "surfaceshader");

            // Set the nodedef attribute (CRITICAL for USD to create child
            // prims)
            shaderNode->setNodeDefString("ND_standard_surface_surfaceshader");

            // Connect shader to material
            mx::InputPtr surfaceShaderInput =
                materialNode->addInput("surfaceshader", "surfaceshader");
            surfaceShaderInput->setNodeName(shader_name);

            spdlog::info(
                "Created default MaterialX document with standard_surface "
                "shader");
        }
        else {
            doc = ptr;
        }
        // Create MaterialXNodeTree with the in-memory document
        std::unique_ptr<NodeTree> tree = std::make_unique<MaterialXNodeTree>(
            system->node_tree_descriptor(), doc);

        system->init(std::move(tree));
        system->set_node_tree_executor(create_node_tree_executor({}));

        return system;
    }

    void set_node_tree_executor(
        std::unique_ptr<NodeTreeExecutor> executor) override
    {
    }

    bool load_configuration(const std::string& config) override
    {
        return true;
    }

    ~MaterialXNodeSystem() override
    {
    }

    void execute(bool is_ui_execution, Node* required_node) const override
    {
    }

    bool get_dirty()
    {
        return get_node_tree()->GetDirty();
    }

    std::shared_ptr<NodeTreeDescriptor> node_tree_descriptor() override
    {
        return descriptor;
    }

   private:
    std::shared_ptr<MaterialXNodeTreeDescriptor> descriptor;
};

class PythonConsoleWidgetFactory : public IWidgetFactory {
   public:
    std::unique_ptr<IWidget> Create(
        const std::vector<std::unique_ptr<IWidget>>& others) override
    {
        // Create Python interpreter
        auto interpreter = python::CreatePythonInterpreter();

        // Create console with capture_log enabled
        ImGui_Console::Options opts;
        opts.show_info = true;
        opts.show_warnings = true;
        opts.show_errors = true;
        opts.capture_log = false;

        auto console = std::make_unique<ImGui_Console>(interpreter, opts);

        // Add some initial messages
        console->Print("==================================");
        console->Print("=== Ruzino Interactive Console ===");
        console->Print("==================================");

        return std::move(console);
    }
};

int main(int argc, char* argv[])
{
    configure_application_logging();
    install_usd_diagnostic_logging();
    const auto terminal_capture_config = load_terminal_capture_config();
    configure_terminal_capture(terminal_capture_config);
    flush_terminal_streams();
    auto window = std::make_unique<Window>();

    // Set MaterialX standard library path using USD's TfSetenv (preferred
    // method)
    std::string mtlx_stdlib = "libraries";
    if (std::filesystem::exists(mtlx_stdlib)) {
        pxr::TfSetenv("PXR_MTLX_STDLIB_SEARCH_PATHS", mtlx_stdlib.c_str());
        spdlog::info("Set PXR_MTLX_STDLIB_SEARCH_PATHS={}", mtlx_stdlib);
    }
    else {
        spdlog::warn("MaterialX stdlib not found at {}", mtlx_stdlib);
    }

    python::initialize();
    // Check for command line arguments to specify USD file
    std::unique_ptr<Stage> stage;
    if (argc > 1) {
        // Use custom stage path from command line
        std::string stage_path = argv[1];
        stage = create_custom_global_stage(stage_path);
    }
    else {
        // Use default stage
        stage = create_global_stage();
    }

#ifdef REAL_TIME
    window->register_function_before_frame(
        [&stage](Window* window) { stage->tick(window->get_elapsed_time()); });
#else

    window->register_function_before_frame(
        [&stage](Window* window) { stage->tick(1.0f / 60.f); });
#endif
    // Add a sphere

    auto usd_file_viewer = std::make_unique<UsdFileViewer>(stage.get());
    auto render = std::make_unique<UsdviewEngine>(stage.get());

    // Use shared_ptr to track render widget lifecycle
    std::shared_ptr<UsdviewEngine*> render_bare_ptr =
        std::make_shared<UsdviewEngine*>(render.get());

    window->register_function_before_frame(
        [render_bare_ptr](Window* window) {
            (void)window;
            if (*render_bare_ptr) {
                (*render_bare_ptr)->ProcessPendingRendererSwitch();
            }
        });

    render->SetCallBack([render_bare_ptr](
                            Window* window, IWidget* render_widget) {
        auto node_system = static_cast<const std::shared_ptr<NodeSystem>*>(
            dynamic_cast<UsdviewEngine*>(render_widget)
                ->emit_create_renderer_ui_control());

        if (node_system) {
            UsdviewEngine* engine =
                dynamic_cast<UsdviewEngine*>(*render_bare_ptr);
            auto current_renderer = engine->GetCurrentRenderer();

            RendererNodeWidgetSettings desc;
            desc.system = *node_system;
            desc.widget_name = "Renderer Graph";
            desc.json_path =
                "../../Assets/" + current_renderer + "/render_nodes_save.json";

            std::unique_ptr<IWidget> node_widget =
                std::move(create_node_imgui_widget(desc));

            window->register_widget(std::move(node_widget));
        }
    });

    window->register_widget(std::move(render));
    window->register_widget(std::move(usd_file_viewer));

    // Register Python Console widget in menu
    auto python_console_factory =
        std::make_unique<PythonConsoleWidgetFactory>();
    window->register_openable_widget(
        std::move(python_console_factory), { "Tools", "Python Console" });
    python::import("GUI_py");
    python::import("stage_py");

    // Add Python reference to window for console access
    python::reference("window", window.get());
    python::reference("stage", stage.get());

    // Register File menu actions
    window->register_menu_action("file_open", [&stage, &window]() {
        auto instance = IGFD::FileDialog::Instance();
        IGFD::FileDialogConfig config;
        config.path = "../../Assets";
        instance->OpenDialog(
            "OpenStageDialog",
            "Open USD Stage",
            "USD Files{.usd,.usda,.usdc,.usdz}",
            config);
    });

    window->register_menu_action("file_save", [&stage]() { stage->Save(); });

    window->register_menu_action("file_save_as", [&stage, &window]() {
        auto instance = IGFD::FileDialog::Instance();
        IGFD::FileDialogConfig config;
        config.path = "../../Assets";
        instance->OpenDialog(
            "SaveStageDialog",
            "Save USD Stage As",
            "USD Files{.usd,.usda,.usdc,.usdz}",
            config);
    });

    // Subscribe to file dialog result events
    window->events().subscribe(
        "file_open_selected", [&stage, &window](const std::string& file_path) {
            flush_terminal_streams();
            if (stage->OpenStage(file_path)) {
                spdlog::info("Successfully opened stage: {}", file_path);
                // Trigger widget recreation
                window->events().emit("stage_reloaded");
            }
            else {
                spdlog::warn("Failed to open stage: {}", file_path);
            }
            flush_terminal_streams();
        });

    window->events().subscribe(
        "file_save_as_selected",
        [&stage](const std::string& file_path) { stage->SaveAs(file_path); });

    // Subscribe to stage reload event to recreate widgets
    window->events().subscribe(
        "stage_reloaded",
        [&stage, &window, render_bare_ptr](const std::string&) {
            flush_terminal_streams();
            spdlog::info("Stage reloaded; resetting existing render widget");
            if (*render_bare_ptr) {
                (*render_bare_ptr)->ReloadStage();
            }
            flush_terminal_streams();
        });

    // Subscribe to material editor events
    window->events().subscribe(
        "material_editor_requested",
        [&stage, &window](const std::string& material_path_str) {
            spdlog::info(
                "Material editor requested for: {}", material_path_str);

            pxr::SdfPath material_path(material_path_str);
            auto material_prim =
                stage->get_usd_stage()->GetPrimAtPath(material_path);

            if (!material_prim) {
                spdlog::error("Material prim not found: {}", material_path_str);
                return;
            }

            // Step 1: Create MaterialX file next to the stage file
            std::string stage_path = stage->GetStagePath();
            std::filesystem::path stage_file(stage_path);
            std::filesystem::path stage_dir = stage_file.parent_path();

            // Get material name from the prim
            std::string material_name = material_prim.GetName();
            std::string mtlx_filename = material_name + ".mtlx";
            std::filesystem::path mtlx_path = stage_dir / mtlx_filename;

            // Check if MaterialX file exists AND material prim already has a
            // reference
            bool has_mtlx_file = std::filesystem::exists(mtlx_path);
            bool has_reference = false;

            // Check if material already has references using GetPrimStack
            auto prim_stack = material_prim.GetPrimStack();
            for (const auto& spec : prim_stack) {
                if (spec->HasReferences()) {
                    has_reference = true;
                    spdlog::info("Material already has reference(s)");
                    break;
                }
            }

            std::shared_ptr<MaterialXNodeSystem> mtlx_system;

            // Only load existing if BOTH file exists AND reference exists
            if (has_mtlx_file && has_reference) {
                // Load existing MaterialX document
                spdlog::info(
                    "Loading existing MaterialX file: {}", mtlx_path.string());
                try {
                    mx::DocumentPtr existing_doc = mx::createDocument();
                    mx::readFromXmlFile(
                        existing_doc, mx::FilePath(mtlx_path.string()));

                    // Create system with existing document using factory method
                    mtlx_system =
                        MaterialXNodeSystem::create_with_default_material(
                            material_name, existing_doc);

                    spdlog::info(
                        "Successfully loaded existing MaterialX document");
                }
                catch (const std::exception& e) {
                    spdlog::error(
                        "Failed to load existing MaterialX file: {}", e.what());
                    has_mtlx_file = false;
                    has_reference = false;
                }
            }

            // Create new if file doesn't exist OR reference doesn't exist
            if (!has_mtlx_file || !has_reference) {
                spdlog::info(
                    "Creating new MaterialX file at: {}", mtlx_path.string());

                mtlx_system = MaterialXNodeSystem::create_with_default_material(
                    material_name);

                // Save the new MaterialX document to file
                auto* mtlx_tree_temp = static_cast<MaterialXNodeTree*>(
                    mtlx_system->get_node_tree());
                mtlx_tree_temp->saveDocument(mx::FilePath(mtlx_path.string()));

                // Add reference to the MaterialX file
                std::string mtlx_relative_path = "./" + mtlx_filename;
                std::string mtlx_material_path_str =
                    "/MaterialX/Materials/" + material_name;

                auto references = material_prim.GetReferences();
                references.ClearReferences();
                references.AddReference(
                    pxr::SdfReference(
                        mtlx_relative_path,
                        pxr::SdfPath(mtlx_material_path_str)));

                spdlog::info(
                    "Added MaterialX reference: {} -> {}",
                    mtlx_relative_path,
                    mtlx_material_path_str);

                stage->get_usd_stage()->Save();
            }

            // Launch MaterialX editor widget
            FileBasedNodeWidgetSettings widget_desc;
            widget_desc.system = mtlx_system;
            widget_desc.json_path =
                (stage_dir / (material_name + "_layout.json")).string();

            std::unique_ptr<IWidget> node_widget =
                std::make_unique<MaterialXNodeTreeWidget>(
                    widget_desc, mtlx_path.string(), material_path_str);

            // Setup callback to save MaterialX file and update USD reference
            // when editor closes
            auto mtlx_path_copy = mtlx_path.string();
            auto material_name_copy = material_name;
            auto stage_ptr = stage.get();
            auto* mtlx_tree =
                static_cast<MaterialXNodeTree*>(mtlx_system->get_node_tree());

            window->register_widget(std::move(node_widget));
        });

    // Subscribe to MaterialX graph change events
    window->events().subscribe(
        "materialx_graph_changed",
        [&stage](const std::string& material_path_str) {
            spdlog::info("MaterialX graph changed for: {}", material_path_str);

            pxr::SdfPath material_path(material_path_str);
            auto material_prim =
                stage->get_usd_stage()->GetPrimAtPath(material_path);

            if (!material_prim) {
                spdlog::error("Material prim not found: {}", material_path_str);
                return;
            }

            // Get the MaterialX file path
            std::string stage_path = stage->GetStagePath();
            std::filesystem::path stage_file(stage_path);
            std::filesystem::path stage_dir = stage_file.parent_path();
            std::string material_name = material_prim.GetName();
            std::filesystem::path mtlx_path =
                stage_dir / (material_name + ".mtlx");

            // Load the MaterialX file to find the surface shader
            mx::DocumentPtr mtlx_doc = mx::createDocument();
            try {
                mx::readFromXmlFile(mtlx_doc, mx::FilePath(mtlx_path.string()));
            }
            catch (const std::exception& e) {
                spdlog::error("Failed to read MaterialX file: {}", e.what());
                return;
            }
            // CRITICAL: Clear USD-layer opinions to let MaterialX reference
            // values show through This solves the opinion strength problem
            // where USD overrides MaterialX

            auto root_layer = stage->get_usd_stage()->GetRootLayer();
            auto material_prim_spec = root_layer->GetPrimAtPath(material_path);

            if (material_prim_spec) {
                // Get all authored attributes from the USD prim (not from
                // reference)
                auto attributes = material_prim.GetAuthoredAttributes();

                for (const auto& attr : attributes) {
                    std::string attr_name = attr.GetName().GetString();

                    // Step 1: Check if this attribute is an input parameter
                    // (starts with "inputs:") and exists in the root layer
                    if (pxr::TfStringStartsWith(attr_name, "inputs:")) {
                        // Check if the attribute is authored in the root layer
                        // (not just coming from the reference)
                        auto attr_spec =
                            material_prim_spec->GetAttributes().get(
                                pxr::TfToken(attr_name));

                        if (attr_spec) {
                            // Step 2: Clear the attribute from the root layer
                            // This removes the USD opinion, allowing MaterialX
                            // reference to win
                            spdlog::info(
                                "Clearing USD opinion for: {} (type: {})",
                                attr_name,
                                attr.GetTypeName().GetCPPTypeName());

                            // Directly use the prim spec handle to remove the
                            // property
                            material_prim_spec->RemoveProperty(attr_spec);
                        }
                    }
                }
            }

            // Re-add the reference to ensure it's up to date
            auto mtlx_relative_path = "./" + material_name + ".mtlx";
            auto mtlx_material_path_str =
                "/MaterialX/Materials/" + material_name;
            auto references = material_prim.GetReferences();
            references.ClearReferences();
            references.AddReference(
                pxr::SdfReference(
                    mtlx_relative_path, pxr::SdfPath(mtlx_material_path_str)));
            // Put on the connection again
            spdlog::info(
                "Re-added MaterialX reference: {} -> {}",
                mtlx_relative_path,
                mtlx_material_path_str);

            //  Find surface shader and sync parameters
            std::string surface_shader_name;
            mx::NodePtr shader_node = nullptr;

            for (auto material_node : mtlx_doc->getMaterialNodes()) {
                auto surfaceshader_input =
                    material_node->getInput("surfaceshader");
                if (surfaceshader_input) {
                    auto connected_node =
                        surfaceshader_input->getConnectedNode();
                    if (connected_node) {
                        surface_shader_name = connected_node->getName();
                        shader_node = connected_node;
                    }
                    else {
                        auto nodename =
                            surfaceshader_input->getAttribute("nodename");
                        if (!nodename.empty()) {
                            surface_shader_name = nodename;
                            shader_node = mtlx_doc->getNode(nodename);
                        }
                    }
                }
                break;
            }

            if (surface_shader_name.empty() || !shader_node) {
                spdlog::warn(
                    "No surface shader found in MaterialX file, skipping USD "
                    "update");
                return;
            }
            // Create USD surface connection
            pxr::SdfPath shader_path =
                material_path.AppendChild(pxr::TfToken(surface_shader_name));
            pxr::SdfPath shader_surface_output_path =
                shader_path.AppendProperty(pxr::TfToken("outputs:surface"));

            pxr::TfToken outputs_surface_token("outputs:surface");
            auto existing_attr =
                material_prim_spec->GetAttributes().get(outputs_surface_token);

            if (existing_attr) {
                auto conn_list = existing_attr->GetConnectionPathList();
                conn_list.ClearEdits();
                conn_list.GetExplicitItems().clear();
                conn_list.GetExplicitItems().push_back(
                    shader_surface_output_path);
            }
            else {
                auto surface_output_attr = pxr::SdfAttributeSpec::New(
                    material_prim_spec,
                    outputs_surface_token,
                    pxr::SdfValueTypeNames->Token);

                if (surface_output_attr) {
                    auto conn_list =
                        surface_output_attr->GetConnectionPathList();
                    conn_list.GetExplicitItems().push_back(
                        shader_surface_output_path);
                }
            }
            stage->get_usd_stage()->Save();
        });
    // Subscribe to document viewer events
    window->events().subscribe(
        "material_doc_viewer_requested",
        [&stage, &window](const std::string& material_path_str) {
            spdlog::info(
                "Material document viewer requested for: {}",
                material_path_str);

            // TODO: Implement document viewer creation
            // This would create a MaterialXDocumentViewer widget
        });

    window->register_function_after_frame(
        [&stage, render_bare_ptr](Window* window) {
            pxr::SdfPath json_path;
            if (stage->consume_editor_creation(json_path)) {
                auto system = create_dynamic_loading_system();
                /* Load the node system */
                auto loaded = system->load_configuration("geometry_nodes.json");
                loaded = system->load_configuration("basic_nodes.json");

                // iterate over path Plugin (not recursively), get all the json
                // and load them

                auto plugin_path = std::filesystem::path("./Plugins");

                if (std::filesystem::exists(plugin_path))
                    for (auto& p :
                         std::filesystem::directory_iterator(plugin_path)) {
                        if (p.path().extension() == ".json") {
                            system->load_configuration(p.path().string());
                        }
                    }

                system->init();
                system->set_node_tree_executor(create_node_tree_executor({}));
                /* Done! */
                UsdBasedNodeWidgetSettings desc;

                desc.json_path = json_path;
                desc.system = system;
                desc.stage = stage.get();

                std::unique_ptr<IWidget> node_widget =
                    std::move(create_node_imgui_widget(desc));
                node_widget->SetCallBack(
                    [&stage, json_path, system, render_bare_ptr](
                        Window*, IWidget*) {
                        GeomPayload geom_global_params;
#ifdef GEOM_USD_EXTENSION
                        geom_global_params.stage = stage->get_usd_stage();
                        geom_global_params.prim_path = json_path;
#endif

                        geom_global_params.has_simulation = false;

                        // Pass pick event from UI to geometry payload
                        if (*render_bare_ptr) {
                            geom_global_params.pick =
                                (*render_bare_ptr)->consume_pick_event();
                        }

                        system->set_global_params(geom_global_params);
                        // if (geom_global_params.pick) {
                        //     system->execute();
                        // }
                    });

                window->register_widget(std::move(node_widget));
            }
        });

    window->register_function_after_frame(
        [&stage](Window* window) { stage->finish_tick(); });
    window->SetMaximized(true);
    window->run();

    unregister_cpp_type();

#ifdef GPU_GEOM_ALGORITHM
#endif
    deinit_gpu_geometry_algorithms();

    window.reset();
    stage.reset();

    uninstall_usd_diagnostic_logging();
    flush_terminal_streams();
    spdlog::default_logger()->flush();
    spdlog::shutdown();
}
