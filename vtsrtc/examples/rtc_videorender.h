#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions_2_0>

class RtcVideoRender : public QOpenGLWidget, protected QOpenGLFunctions_2_0 {
	Q_OBJECT

public:
	explicit RtcVideoRender();
	~RtcVideoRender();

	void OnFrame(const char* sourceid, size_t width, size_t height, size_t dimension,
		const unsigned char* buffer, size_t sz_buffer);

protected:
	void initializeGL() override;
	void paintGL() override;
	void resizeGL(int width, int height) override;

private:
	size_t width_ = 0, height_ = 0;
	size_t new_width_ = 0, new_height_ = 0;
	unsigned char* framebuffer_;
	GLuint texture_;
};
