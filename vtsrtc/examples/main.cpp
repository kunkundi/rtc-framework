#include "rtc_widget.h"
#include <QApplication>
#if defined  __aarch64__
#include <QWidget>
#endif

int main(int argc, char* argv[]) {
#if defined  __aarch64__
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);
#endif

	QApplication app(argc, argv);

	QString exe_path = QCoreApplication::applicationDirPath();
	std::string rtc_config_filepath = std::string(exe_path.toLocal8Bit()) + "/rtc.cfg";
	QString pcmdata_filepath = exe_path + "/8k16bit.pcm";
	QString yuv_data_dir = exe_path + QString("/yuvfiles");


	RtcWidget rtc_widget(rtc_config_filepath, pcmdata_filepath, yuv_data_dir);
	rtc_widget.show();

	return app.exec();
}
