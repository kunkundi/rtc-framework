#pragma once

#include <string>

namespace HttpStatus {
	const std::string status_field = "status";
	const std::string message_field = "message";
	const std::string data_field = "data";

	enum Code {
		OK = 0,
		InternalError = 1001,
		ApiNotExisted = 1002,
		ApiDeprecated = 1003,
		ArgumentIncorrect = 2001,
		ArgumentNumberIncorrect = 2002,
		ArgumentTypeIncorrect = 2003,
		BodyParameterJsonInvalid = 2101,
		ParameterIncorrect = 2102,
		ParameterNumberIncorrect = 2103,
		ParameterTypeIncorrect = 2104,
		IPInvalid = 2201,
		MACInvalid = 2202,
		EmailInvalid = 2203,
		UuidInvalid = 2204,
		TcpPortInvalid = 2205,
		LongitudeInvalid = 2206,
		LatitudeInvalid = 2207,
		TimestampInvalid = 2208,
		DateInvalid = 2209,
		ComputerLocalPathInvalid = 2210,
		ComputerSharedPathInvalid = 2211,
		UserNameInvalid = 2212,
		PasswordInvalid = 2213,
		PhoneInvalid = 2214,
		UploadFileNotExist = 2301,
		UploadFileOversized = 2302,
		UnsupportedFileFormat = 2303,
		FailedToCreateUploadFolder = 2304,
		DownloadFileNotExist = 2305,
		DoNotNeedToUpload = 2310,
		RoomNotExisted = 3001,
		RoomAlreadyExisted = 3002,
		SessionidAlreadyInRoom = 3003,
	};

	inline std::string reason_phrase(Code code) {
		switch (code) {
		case Code::OK: return "OK";

		case Code::InternalError: return "Internal error";
		case Code::ApiNotExisted: return "Api not existed. You can call /api/protocol_version by HTTP GET to get the protocol in advance.";
		case Code::ApiDeprecated: return "Api deprecated";

		case Code::ArgumentIncorrect: return "Argument incorrect";
		case Code::ArgumentNumberIncorrect: return "Argument number incorrect";
		case Code::ArgumentTypeIncorrect: return "Argument type incorrect";

		case Code::BodyParameterJsonInvalid: return "Body parmeter is invalid json format";
		case Code::ParameterIncorrect: return "Parameter incorrect";
		case Code::ParameterNumberIncorrect: return "Parameter number incorrect";
		case Code::ParameterTypeIncorrect: return "Parameter type incorrect";

		case Code::IPInvalid: return "IP address is invalid";
		case Code::MACInvalid: return "MAC address is invalid";
		case Code::EmailInvalid: return "Email is invalid";
		case Code::UuidInvalid: return "Uuid is invalid";
		case Code::TcpPortInvalid: return "Tcp port is invalid";
		case Code::LongitudeInvalid: return "Longitude is invalid";
		case Code::LatitudeInvalid: return "Latitude is invalid";
		case Code::TimestampInvalid: return "Timestamp is invalid";
		case Code::DateInvalid: return "Date is invalid";
		case Code::ComputerLocalPathInvalid: return "Computer local path is invalid";
		case Code::ComputerSharedPathInvalid: return "Computer shared path is invalid";
		case Code::UserNameInvalid: return "Username is invalid";
		case Code::PasswordInvalid: return "Password is invalid";
		case Code::PhoneInvalid: return "Phone is invalid";

		case Code::UploadFileNotExist: return "Upload file not exist";
		case Code::UploadFileOversized: return "Upload file oversized";
		case Code::UnsupportedFileFormat: return "Unsupported file format";
		case Code::FailedToCreateUploadFolder: return "Failed to create the upload folder";
		case Code::DoNotNeedToUpload: return "Donot need to upload";
		case Code::DownloadFileNotExist: return "Download file not exist";

		case Code::RoomNotExisted: return "Room not existed";
		case Code::RoomAlreadyExisted: return "Room already existed";
		case Code::SessionidAlreadyInRoom: return "Sessionid already in room";

		default: return std::string();
		}
	}
}
