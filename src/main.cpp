#include "App.hpp"

#include <iostream>
#include <QApplication>
#include <opencv2/core/utils/logger.hpp>

int main(int argc, char** argv) {
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_WARNING);
    try {
        QApplication qapp(argc, argv);
        pubg::App app;
        return app.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
