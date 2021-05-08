#include "rtc_widget.h"
#include <QApplication>

int main(int argc, char* argv[]) {
	QApplication app(argc, argv);

	QString exe_path = QCoreApplication::applicationDirPath();
	std::string rtc_config_filepath = std::string(exe_path.toLocal8Bit()) + "/rtc.cfg";
	QString yuv_data_dir = exe_path + QString("/yuvfiles");

	RtcWidget rtc_widget(rtc_config_filepath, yuv_data_dir);
	rtc_widget.show();

	return app.exec();
}