#include "rtc_videorender.h"
#include <iostream>

RtcVideoRender::RtcVideoRender() {
	this->setMinimumSize(600, 400);
}

RtcVideoRender::~RtcVideoRender() {
	glDeleteTextures(1, &texture_);
}

void RtcVideoRender::OnFrame(const char* sourceid, size_t width, size_t height, size_t dimension,
	const unsigned char* buffer, size_t sz_buffer) {
	// std::cout << sourceid << ", " << width << ", " << height << ", " << sz_buffer << std::endl;
	new_width_ = width;
	new_height_ = height;
	framebuffer_ = const_cast<unsigned char*>(buffer);

	update();
}

void RtcVideoRender::initializeGL() {
	initializeOpenGLFunctions();

	glGenTextures(1, &texture_);
}

void RtcVideoRender::paintGL() {
	if (new_width_ <= 0 || new_height_ <= 0) {
		return;
	}

	if (width_ != new_width_ || height_ != new_height_) {
		width_ = new_width_;
		height_ = new_height_;

		this->setFixedSize(width_, height_);

		glBindTexture(GL_TEXTURE_2D, texture_);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width_, height_, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, framebuffer_);
	}
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, texture_);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, framebuffer_);


	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, texture_);

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glLoadIdentity();

	// do horizontal flip
	glBegin(GL_QUADS);
	glTexCoord2f(0.0f, 0.0f);
	glVertex3f(1.0f, 0.0f, 0.0f);

	glTexCoord2f(0.0f, 1.0f);
	glVertex3f(1.0f, 1.0f, 0.0f);

	glTexCoord2f(1.0f, 1.0f);
	glVertex3f(0.0f, 1.0f, 0.0f);

	glTexCoord2f(1.0f, 0.0f);
	glVertex3f(0.0f, 0.0f, 0.0f);
	glEnd();

	glBindTexture(GL_TEXTURE_2D, 0);
	glFlush();
}

void RtcVideoRender::resizeGL(int width, int height) {
	glViewport(0, 0, width, height);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glClearColor(.0f, .0f, .0f, 1.0f);
	glOrtho(0.0f, 1.0f, 1.0f, 0.0f, -1.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
}
