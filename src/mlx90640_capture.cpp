#include <stdint.h>
#include <iostream>
#include <cstring>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include "MLX90640/MLX90640_API.h"
#include "mlx90640_buffer.pb.h"
#include <sw/redis++/redis++.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#define SI_CONVERT_ICU
#include "SimpleIni.h"

#include "mlx90640_config.h"

namespace fs = std::filesystem;
using namespace sw::redis;
using namespace std;
/*
 * mlx90640_capture
 * ======
 * Capture max resolution data from the camera.
 * Interpolates over bad pixels
 * Converts to temperatures (assumed emissivity of 0.8)
 * Encodes each frame as a protobuf (Mlk90640Frame)
 * When each stringified message is written out
 * the schema version (uint64_t) and message length(uint64_t) 
 * are prefixed onto the front of it.
 * 
 */


#define MLX_I2C_ADDR 0x33

// Valid frame rates are 1, 2, 4, 8, 16, 32 and 64
// The i2c baudrate is set to 1mhz to support these
#define FPS 8
#define FRAME_TIME_MICROS (1000000/FPS)
#define SKIP_FRAMES 2
#define MAX_COUNT 0
#define WRITE_BATCH_SIZE FPS

/*
 * Protobuf doesn't help with message schema versioning.  
 * Since aggregated messages need each need to be preceeded by the message length
 * add a schema version value to this header
 */

// Despite the framerate being ostensibly FPS hz
// The frame is often not ready in time
// This offset is added to the FRAME_TIME_MICROS
// to account for this.
#define OFFSET_MICROS 850

int main(int argc, char *argv[]){
	GOOGLE_PROTOBUF_VERIFY_VERSION;
	const uint64_t Mlx90640Fframe_schema_version=1;
	string device_name("MLX90640");
	string device_id("0");

	const float emissivity = 0.8;
	/* 32 is just a random number plucked from thin air. */
	const auto maxrpt=32;
	/* If there is contention on the i2c bus give other devices
	   time to get out of our way.  50 milliseconds is a wet
	   finger in the air guestimate based on the read times of 
	   other I2C devices sharing the same i2c bus.
	   */
	const auto i2c_sleep = chrono::milliseconds(50);


	static uint16_t eeMLX90640[832];
	uint16_t frame[834];

	float mlx90640To[768];
	// float data[768];
	float eTa;

	auto fps = FPS;
	auto skip_frames = SKIP_FRAMES;
	auto count = 0;
	auto max_batch_size=FPS;
	auto max_batch_size_set = 0;
	auto maxcount = MAX_COUNT;
	string outfile;

	auto write_to_stdout = 0;
	auto write_to_file = 0;
	auto write_to_redis = 0;
	auto rsp=0;
	auto rpt=maxrpt;
	auto frame_time_micros = FRAME_TIME_MICROS;
	char *p;
	string pmsg; 

	string redis_host;
	string redis_data_index;
	string redis_data_key("...:DATA");
	string redis_password;
	string redis_user;
	auto redis_data_key_set=0;
	auto redis_data_index_set=0;

	char hostname[HOST_NAME_MAX];
	gethostname(hostname, HOST_NAME_MAX);
	struct passwd *pw = getpwuid(getuid());
	const fs::path home_dir(pw->pw_dir);
	const fs::path cfg_filename(".mlx90640_capture.cfg");
	auto  cfg_path = home_dir / cfg_filename;
	const char* default_key=device_name.c_str();
	CSimpleIniA ini;
	if (fs::exists(cfg_path)) {
		ini.SetUnicode();
		SI_Error rc = ini.LoadFile(cfg_path.u8string().c_str());
		if (rc < 0) { 
			fprintf(stderr, "Unable to read configuration file.");
			return 1;
		};

		if (ini.SectionExists(default_key)) {
			if ini.KeyExists(default_key, "FPS") {
				auto v = ini.GetLongValue(default_key,"FPS",FPS, 0);
				fps = v;
			}
			if ini.KeyExists(default_key, "SKIP_FRAMES") {
				auto v = ini.GetLongValue(default_key,"SKIP_FRAMES", SKIP_FRAMES, 0);
				skip_frame = v;
			}
			if ini.KeyExists(default_key, "MAX_COUNT") {
				auto v = ini.GetLongValue(default_key,"MAX_COUNT", MAX_COUNT, 0);
				maxcount = v;
			}
			if ini.KeyExists(default_key, "WRITE_BATCH_SIZE") {
				auto v = ini.GetLongValue(default_key,"WRITE_BATCH_SIZE", WRITE_BATCH_SIZE, 0);
				maxcount = v;
			}
			if ini.KeyExists(default_key, "WRITE_TO_STDOUT") {
				auto v = ini.GetBoolValue(default_key,"WRITE_TO_STDOUT", WRITE_BATCH_SIZE, 0);
				write_to_stdout = v;
			}
			if ini.KeyExists(default_key, "WRITE_TO_FILE") {
				auto v = ini.GetBoolValue(default_key,"WRITE_TO_FILE", WRITE_BATCH_SIZE, 0);
				write_to_file = v;
				if (write_to_file && ini.KeyExists(default_key, "OUTFILE")) {
					auto v2 = ini.GetValue(default_key,"OUTFILE", "mlx90640_capture.bin", 0);
					outfile.assign(v2);
				}
			}
			if ini.KeyExists(default_key, "WRITE_TO_REDIS") {
				auto v = ini.GetBoolValue(default_key,"WRITE_TO_REDIS", WRITE_BATCH_SIZE, 0);
				write_to_redis = v;
				if (write_to_redis) {
					if (ini.KeyExists(default_key, "REDIS_HOST")) {
						auto v2 = ini.GetValue(default_key,"REDIS_HOST", "127.0.0.1", 0);
						redis_host.assign(v2);
					}
					if (ini.KeyExists(default_key, "REDIS_USER")) {
						auto v2 = ini.GetValue(default_key,"REDIS_USER", "default", 0);
						redis_user.assign(v2);
					}
					if (ini.KeyExists(default_key, "REDIS_KEY")) {
						auto v2 = ini.GetValue(default_key,"REDIS_KEY");
						if (v2 != NULL) {
							redis_data_key.assign(v2);
							redis_data_key_set = 1;
						}
					}
					if (ini.KeyExists(default_key, "REDIS_PASSWORD")) {
						auto v2 = ini.GetValue(default_key,"REDIS_PASSWORD","",0);
						redis_password.assign(v2);
					}
					if (ini.KeyExists(default_key, "REDIS_INDEX")) {
						auto v2 = ini.GetValue(default_key,"REDIS_INDEX","MLX_90640:CURRENT_TIMESERIES",0);
						redis_data_index.assign(v2);
						redis_data_index_set=1;
					}
				}
			}
		}

	}


	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i],"-f")==0 || strcmp(argv[i],"--fps")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Framerate argument without value.\n");
				return 1;
			}
			fps = strtoul(argv[i+1], &p, 0);
			if (errno !=0 || *p != '\0' ) {
				fprintf(stderr, "Invalid framerate.\n");
				return 1;
			}
		}
		if (strcmp(argv[i],"-s")==0 || strcmp(argv[i],"--skip")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Skip frames argument without value.\n");
				return 1;
			}
			skip_frames = strtoul(argv[i+1], &p, 0);
			if (errno !=0 || *p != '\0') {
				fprintf(stderr, "Invalid skip frames value.");
				return 1;
			}
		}
		if (strcmp(argv[i],"-c")==0 || strcmp(argv[i],"--count")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Count argument without value.\n");
				return 1;
			}
			maxcount = strtoul(argv[i+1], &p, 0);
			if (errno !=0 || *p != '\0') {
				fprintf(stderr, "Invalid count");
				return 1;
			}
		}
		if (strcmp(argv[i],"-b")==0 || strcmp(argv[i],"--batch")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Batch argument without value.\n");
				return 1;
			}
			max_batch_size_set = 1;
			max_batch_size = strtoul(argv[i+1], &p, 0);
			if (errno !=0 || *p != '\0') {
				fprintf(stderr, "Invalid batch size");
				return 1;
			}
		}
		if (strcmp(argv[i],"--stdout")==0 || (strcmp(argv[i],"--")==0 && i==argc-1 ) ) {
			write_to_stdout=1;
		}
		if (strcmp(argv[i],"-o")==0 || strcmp(argv[i],"--output")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Output argument without value.\n");
				return 1;
			}
			write_to_file=1;			
			outfile.assign(argv[i+1]);
		}
		if (strcmp(argv[i],"-r")==0 || strcmp(argv[i],"--redis")==0) {
			write_to_redis=1;			
		}

		if (strcmp(argv[i],"-h")==0 || strcmp(argv[i],"--host")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Redis host argument without value.\n");
				return 1;
			}
			redis_host.assign(argv[i+1]);
		}
		if (strcmp(argv[i],"-k")==0 || strcmp(argv[i],"--key")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Redis key argument without value.\n");
				return 1;
			}
			redis_data_key.assign(argv[i+1]);
			redis_data_key_set=1;
		}
		if (strcmp(argv[i],"-i")==0 || strcmp(argv[i],"--index")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Redis index argument without value.\n");
				return 1;
			}
			redis_data_index.assign(argv[i+1]);
			redis_data_index_set=1;
		}
		if (strcmp(argv[i],"--version")==0) {
			cout << argv[0] << " Version " << mlx90640_capture_VERSION_MAJOR << "." << mlx90640_capture_VERSION_MINOR << endl;
			return 0;
		}
		if (strcmp(argv[i],"--help")==0) {
			cout << argv[0] << " Version " << mlx90640_capture_VERSION_MAJOR << "." << mlx90640_capture_VERSION_MINOR << endl;
			cout << "\t-f|--fps    n[int| 0, 1, 2, 4, (8), 16, 32]  - Framerate." << endl;
			cout << "\t-s|--skip   n[int| (2)]     - Skip this many frames on startup." << endl;
			cout << "\t-c|--count  n[int| (0)]     - Only record this many frames (0 => don't stop)." << endl;
			cout << "\t-b|--batch  n[int| (FPS)]   - Write data out in batches of this size." << endl;
			cout << "\t-o|--output n[string|None]  - Write data to the given filename." << endl;
			cout << "\t-r|--redis  - Write data to the given filename." << endl;
			cout << "\t-h|--host   - Redis host." << endl;
			cout << "\t-k|--key    - Write to this Redis key." << endl;
			cout << "\t-i|--index  - Get the redis key from redis by reading this key." << endl;
			cout << "\t--|--stdout - Write data to stdout" << endl;
			return 0;
		}
	}

	if ( (fps & (fps - 1)) != 0 || fps > 64) {
		fprintf(stderr, "Invalid framerate.  Valid values are 0, 1, 2, 4, 8, 16, 32.\n");
		return 1;
	}
	if (maxcount && max_batch_size > maxcount) {
		max_batch_size_set=1;
		max_batch_size = maxcount;
	}
	if (!max_batch_size_set) {
		max_batch_size = fps;
	}

	std::string::size_type pos = 0u;
	if ((pos = redis_data_key.find("...", pos)) != std::string::npos){
		string redis_key_prefix(hostname);
		redis_data_key.append(":");
		redis_data_key.append(device_name);
		redis_data_key.append(":");
		redis_data_key.append(device_id);
		redis_data_key.replace(pos,pos+3,redis_key_prefix)
	}

	auto frame_time = chrono::microseconds(frame_time_micros + OFFSET_MICROS);
	mlx90640::Mlx90640Frame pframes[max_batch_size];

	rpt=maxrpt;
	do {
		rsp = MLX90640_SetDeviceMode(MLX_I2C_ADDR, 0);
		this_thread::sleep_for(i2c_sleep);
	} while (rsp == -1 && --rpt);
	if (rsp < 0) {
		fprintf(stderr, "MLX90640_SetDeviceMode: Too many NAK from I2C commands.\n");
		return 1;
	}

	rpt=maxrpt;
	do {
		rsp = MLX90640_SetDeviceMode(MLX_I2C_ADDR, 0);
		this_thread::sleep_for(i2c_sleep);
	} while (rsp == -1 && --rpt);
	if (rsp < 0) {
		fprintf(stderr, "MLX90640_SetDeviceMode: Too many NAK from I2C commands.\n");
		return 1;
	}

	rpt=maxrpt;
	do {
		rsp = MLX90640_SetSubPageRepeat(MLX_I2C_ADDR, 0);
		this_thread::sleep_for(i2c_sleep);
	} while (rsp == -1 && --rpt);
	if (rsp < 0) {
		fprintf(stderr, "MLX90640_SetSubPageRepeat: Too many NAK from I2C commands.\n");
		return 1;
	}

	switch(fps){
		case 1:
			MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b001);
			break;
		case 2:
			MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b010);
			break;
		case 4:
			MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b011);
			break;
		case 8:
			MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b100);
			break;
		case 16:
			MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b101);
			break;
		case 32:
			MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b110);
			break;
		case 64:
			MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b111);
			break;
		default:
			fprintf(stderr, "Unsupported framerate: %d\n", fps);
			return 1;
	}

	rpt=maxrpt;
	do {
		rsp = MLX90640_SetChessMode(MLX_I2C_ADDR);
		if (rsp == -2 ) {
			fprintf(stderr, "MLX90640_SetChessMode: Error setting chess mode.");
			return 1;
		}
		this_thread::sleep_for(i2c_sleep);
	} while (rsp==-1 and --rpt);
	if (rsp < 0) {
		fprintf(stderr, "MLX90640_SetChessMode: Too many NAK from I2C commands.\n");
		return 1;
	}

	paramsMLX90640 mlx90640;

	rpt=maxrpt;
	do {
		rsp = MLX90640_DumpEE(MLX_I2C_ADDR, eeMLX90640);
		this_thread::sleep_for(i2c_sleep);
	} while (rsp==-1 and --rpt);
	if (rsp < 0) {
		fprintf(stderr, "MLX90640_DumpEE: Too many NAK from I2C commands.\n");
		return 1;
	}

	rpt=maxrpt;
	do {
		rsp = MLX90640_SetResolution(MLX_I2C_ADDR, 0x03);
		if (rsp == -2 ) {
			fprintf(stderr, "Error setting resolution.");
			return 1;
		}
		this_thread::sleep_for(i2c_sleep);
	} while (rsp==-1 and --rpt);
	if (rsp < 0) {
		fprintf(stderr, "MLX90640_SetResolution: Too many NAK from I2C commands.\n");
		return 1;
	}

	rpt=maxrpt;
	do{
		rsp = MLX90640_ExtractParameters(eeMLX90640, &mlx90640);
		if (rsp == -7 ) {
			fprintf(stderr, "EEPROM data at the specified location is not a valid MLX90640 EEPROM");
			return 1;
		}
		rpt--;
		this_thread::sleep_for(i2c_sleep);
	} while (rsp==-1 and --rpt);
	if (rsp < 0) {
		fprintf(stderr, "MLX90640_ExtractParameters: Too many NAK from I2C commands.\n");
		return 1;
	}

	// Use the difference between system_clock epoch (UNIX) and steady_clock epoch
	// (unspecified) to get a reference for converting steady_clock timestamps 
	// (we cannot have backsteps in time) to a nominal unix-epoch timestamp.
	//
	const auto reference_epoch_ns = (chrono::duration_cast<chrono::nanoseconds>( chrono::system_clock::now().time_since_epoch() ) - chrono::duration_cast<chrono::nanoseconds>( chrono::steady_clock::now().time_since_epoch())).count();
	ostringstream ss;
	while (!maxcount || count < maxcount){
		auto batch_count=0;
		while(batch_count < max_batch_size && ( !maxcount || count <= maxcount)) {
			auto start = chrono::steady_clock::now();
			rsp = MLX90640_GetFrameData(MLX_I2C_ADDR, frame);
			if (rsp == -1 || rsp == -8 || skip_frames) {
				cerr << "rsp=" << rsp << endl;
				// If there is contention on the i2c bus at this point
				// try dropping one frame at a time.
				// -1 == NACK during communications.
				// -8 == GetFrameData could not read entire frame.
				//  Wait a frame before trying again.
				if (skip_frames) {
					--skip_frames;
				}
				this_thread::sleep_for(chrono::microseconds(frame_time - chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - start)));
				continue;
			}

			MLX90640_InterpolateOutliers(frame, eeMLX90640);
			eTa = MLX90640_GetTa(frame, &mlx90640); // Sensor ambient temprature
			MLX90640_CalculateTo(frame, &mlx90640, emissivity, eTa, mlx90640To); //calculate temprature of all pixels, base on emissivity of object

			uint64_t ts = (uint64_t)( chrono::duration_cast<chrono::nanoseconds>( start.time_since_epoch() ).count() + reference_epoch_ns );

			// MLX90640 data comes out with the first element of the array being the top right pixel.
			// Reorganize to get the top left pixel as the first element of the array.
			pframes[batch_count].Clear();
			pframes[batch_count].set_timestamp(ts);
			pframes[batch_count].set_data_order(mlx90640::Mlx90640Frame::LR);
			for(int y = 0; y < 24; y++){
				for(int x = 0; x < 32; x++){
					auto j = 32 * (23-y) + x;
					pframes[batch_count].add_data(mlx90640To[j]);
				}
			}

			// Need to send the data somewhere
			this_thread::sleep_for(chrono::microseconds(frame_time - chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - start)));
			++batch_count;
			++count;
			// cout << "batch_count/max_batch_size" << batch_count << "/" << max_batch_size << endl;
			// cout << "count/maxcount" << count << "/" << maxcount << endl;
		}
		if (write_to_stdout || write_to_file) {
			ss.clear();
			ss.str(string());
			for(auto i=0; i < batch_count; ++i) {
				uint64_t slen = (uint64_t) pframes[i].ByteSizeLong();
				ss.write((char const*) &Mlx90640Fframe_schema_version, sizeof(Mlx90640Fframe_schema_version));
				ss.write((char const*) &slen, sizeof(slen));
				pframes[i].SerializeToOstream(&ss);
				pframes[i].Clear();
			}
			if (write_to_file) {
				ofstream out(outfile, ios::binary | ios::out | ios::app);
				out << ss.str() << flush;
				out.close();

			}
			if (write_to_stdout) {
				cout << ss.str() << flush;

			}
		}
		if (write_to_redis) {
			cout << "TBD" << endl;
		}
	}
	google::protobuf::ShutdownProtobufLibrary();
	return 0;
}
