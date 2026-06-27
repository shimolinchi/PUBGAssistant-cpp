#include "App.hpp"

#include <iostream>
#include <QApplication>
#include <opencv2/core/utils/logger.hpp>

#ifdef _WIN32
#include <windows.h>
#include <shellscalingapi.h>
#endif

namespace {

void enablePerMonitorDpiAwareness() {
#ifdef _WIN32
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return;
    }
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
#endif
}

} // namespace

int main(int argc, char** argv) {
    enablePerMonitorDpiAwareness();
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_WARNING);
    try {
        int code = 0;
        {
            QApplication qapp(argc, argv);
            {
                pubg::App app;
                code = app.run();
            }
        }
        return code;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
