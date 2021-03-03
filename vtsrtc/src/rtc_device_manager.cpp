#include "rtc_device_manager.h"
#include "rtc_videocapturer.hpp"
#include <iostream>

vts_rtc::VideoDevices RtcDeviceManager::GetVideoDevices() {
	std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> vcm_deviceinfo(
		webrtc::VideoCaptureFactory::CreateDeviceInfo());

	if (!vcm_deviceinfo) { return {}; }

	auto num_of_devices = vcm_deviceinfo->NumberOfDevices();
	if (num_of_devices <= 0) { return {}; }

	vts_rtc::VideoDevices video_devices;
	char device_name[256];  // friendly name of the capture device
	char device_uniqueid[256];
	char product_uniqueid[256];
	for (size_t idx = 0; idx < num_of_devices; ++idx) {
		vcm_deviceinfo->GetDeviceName(idx, device_name, sizeof(device_name), device_uniqueid, 
			sizeof(device_uniqueid), product_uniqueid, sizeof(product_uniqueid));
		
		auto num_of_capabilities = vcm_deviceinfo->NumberOfCapabilities(device_uniqueid);
		std::vector<vts_rtc::VideoDeviceCapability> device_capabilities;
		for (size_t cap_idx = 0; cap_idx < num_of_capabilities; ++cap_idx) {
			webrtc::VideoCaptureCapability inner_capability;
			vcm_deviceinfo->GetCapability(device_uniqueid, cap_idx, inner_capability);
			device_capabilities.emplace_back(vts_rtc::VideoDeviceCapability {
				static_cast<size_t>(inner_capability.width),
				static_cast<size_t>(inner_capability.height),
				static_cast<size_t>(inner_capability.maxFPS)
				// TO DO
				// support video_type
				});
		}

		video_devices.emplace_back(vts_rtc::VideoDevice {
			idx, 
			std::string(device_name),
			std::string(device_uniqueid), 
			std::string(product_uniqueid),
			device_capabilities
			});
	}
	return video_devices;
}

void RtcDeviceManager::AddVideoCapturer(size_t device_index, const vts_rtc::VideoDeviceCapability& device_capability) {
	auto video_devices = this->GetVideoDevices();

	if (device_index >= video_devices.size()) {
		LOG_ERROR("Get video track sources, device_index (%llu) out of range, video_devices size: %llu", device_index, video_devices.size());
		return;
	}

	auto label = std::string("camera_capturer") + std::to_string(device_index);
	auto device_uniqueid = video_devices[device_index].device_uniqueid;
	auto track_source = RtcCameraCapturerTrackSource::Create(label, device_uniqueid, device_capability);
	if (track_source) {
		track_sources_.emplace_back(track_source);
	}
}

RtcDeviceManager::RtcCCTrackSources RtcDeviceManager::GetVideoTrackSources() const {
	return track_sources_;
}
