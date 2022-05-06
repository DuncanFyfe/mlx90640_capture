#define SI_CONVERT_ICU
#include <stdint.h>
#include <iostream>
#include <cstring>
#include <filesystem>
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
#include "SimpleIni.h"

#include "mlx90640_config.h"

namespace fs = std::filesystem;
namespace chronos = std::chrono;
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

#define DEBUGME
#ifndef DEBUGME
#define DEBUG(x) 
#else
#define DEBUG(x) cout << "DEBUG: " << x << endl;
#endif

// Despite the framerate being ostensibly FPS hz
// The frame is often not ready in time
// This offset is added to the FRAME_TIME_MICROS
// to account for this.
#define OFFSET_MICROS 850

bool strtobool(const string& s, const bool dflt = false) {
        const char* sc=s.c_str();
        if (!strcmp(sc,"Y") || !strcmp(sc,"y") || 
            !strcmp(sc,"T") || !strcmp(sc,"t") ||
            !strcmp(sc,"1") || 
            !strcmp(sc,"YES") || !strcmp(sc,"yes") || !strcmp(sc,"Yes") ||    
            !strcmp(sc,"TRUE") || !strcmp(sc,"true") ||  !strcmp(sc,"True") ||
            !strcmp(sc,"ON") || !strcmp(sc,"on") || !strcmp(sc,"On") ) {
                return true;
        } else if (!strcmp(sc,"N") || !strcmp(sc,"n") || 
            !strcmp(sc,"F") || !strcmp(sc,"f") ||
            !strcmp(sc,"1") || 
            !strcmp(sc,"No") || !strcmp(sc,"no") || !strcmp(sc,"No") ||    
            !strcmp(sc,"FALSE") || !strcmp(sc,"false") ||  !strcmp(sc,"False") ||    
            !strcmp(sc,"OFF") || !strcmp(sc,"off") || !strcmp(sc,"off") ) {
                return false;    
        } else {
                return dflt;
        }       
}

void expand_redis_key(string& redis_key, const string sensor_identifier) {
	std::string::size_type pos = 0u;
	if ((pos = redis_key.find("...", pos)) != std::string::npos){
		redis_key.replace(pos,3,sensor_identifier);
	}
}

int main(int argc, char *argv[]){
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	/*
	 * Protobuf doesn't help with message schema versioning.  
	 * Since aggregated messages need each need to be preceeded by the message length
	 * add a schema version value to this header
	 */
	const uint64_t Mlx90640Fframe_schema_version=1;

	/* 
	 * maxrpt: Maximum number of retries of I2C commands (typically due to I2C bus NAK).
	 */
	const auto maxrpt=32;
	/* 
	 *  i2c_sleep: If there is contention on the i2c bus then
	 *  sleep for this period before trying again to give other devices
	 *  time to get out of the way.  43 milliseconds is a prime wet
	 *  finger in the air guestimate based on the read times of 
	 *  other I2C devices sharing the same i2c bus.
	 */
	auto i2c_sleep = chronos::milliseconds(43);

	/*
	 * Used to construct device dependant redis keys.
	 * device_name identified the type/class of device recording data
	 * device_num  distinguishes multiple devices attached to the 
	 * same host (defaults to 0).
	 */
	string device_name("MLX90640");
	string device_id("0");
	float emissivity = 0.8;
	bool debug = false;

	static uint16_t eeMLX90640[832];
	uint16_t frame[834];
	float mlx90640To[768];
	float eTa;

	// If auto is used here then the output of strtoul later 
	// gets converted to signed.  Force the unsigned.
	unsigned long fps = 8;
	unsigned long skip_frames = 2;
	unsigned long batch_size=fps;
	unsigned long batch_size_set = 0;
	unsigned long count = 0;
	unsigned long max_count = 0;
	string outfile;

	auto write_to_stdout = 0;
	auto write_to_file = 0;
	auto write_to_redis = 0;
	auto rsp=0;
	auto rpt=maxrpt;
	char *p;

	string redis_host("localhost");
	string redis_user("default");
	string redis_password;
	// redis_sensor_key is the redis key which identifies the timeseries 
	// elements in redis to which sensor readings are written.  
	// redis_config_key is the redis key which identifies the timeseries 
	// elements in redis to which sensor configuration and calibration data are written.  
	// If redis_sensor_index is set the value of redis_sensor_key is assigned
	// the value read fro that key.  The index is re-read for each batch of
	// data.
	// If redis_config_index is set the value of redis_sensor_key is assigned
	// the value read fro that key.  At present the calibration and configuration are
	// static across an execution of the program so this index is read on startup.
	//
	// For both the data key, calibration key and the index key the first instance of "..." in the key will be
	// expanded later to "node:device_name:device_id" 
	// For example: "...:SENSOR" becomes "node-123:MLX90640:0:SENSOR"
	// and "10:...:SENSOR" becomes "10:node-123:MLX90640:0:SENSOR"
	//
	string redis_sensor_index;
	string redis_sensor_key("...:SENSOR");
	string redis_config_index;
	string redis_config_key("...:CONFIG");

	char hostname[HOST_NAME_MAX];
	gethostname(hostname, HOST_NAME_MAX);
	struct passwd *pw = getpwuid(getuid());
	const fs::path home_dir = pw->pw_dir;
	const fs::path cfg_filename = ".mlx90640_capture.cfg";
	auto  cfg_path = home_dir / cfg_filename;
	const char* default_section=device_name.c_str();

	/*
	 * Read the configuration file if it is available.
	 */

	CSimpleIniA ini;
	error_code e;
	if (fs::exists(cfg_path, e)) {
		ini.SetUnicode();
		SI_Error rc = ini.LoadFile(cfg_path.u8string().c_str());
		if (rc < 0) { 
			fprintf(stderr, "Unable to read configuration file.");
			return 1;
		};

		if (ini.GetSection(default_section) != NULL) {
			errno = 0;
			auto v2 = ini.GetValue(default_section, "FPS");
			auto v2len = 0;
			if (v2 != NULL) {
				fps = strtoul(v2, &p, 0);
				if (errno !=0 || *p != '\0' ) {
					fprintf(stderr, "Invalid FPS.\n");
					return 1;
				}
			}
			v2 = ini.GetValue(default_section, "SKIP_FRAMES");
			if (v2 != NULL) {
				skip_frames = strtoul(v2, &p, 0);
				if (errno !=0 || *p != '\0' ) {
					fprintf(stderr, "Invalid SKIP_FRAMES.\n");
					return 1;
				}
			}
			v2 = ini.GetValue(default_section, "MAX_COUNT");
			if (v2 != NULL) {
				max_count = strtoul(v2, &p, 0);
				if (errno !=0 || *p != '\0' ) {
					fprintf(stderr, "Invalid MAX_COUNT.\n");
					return 1;
				}
			}
			v2 = ini.GetValue(default_section, "BATCH_SIZE");
			if (v2 != NULL) {
				batch_size = strtoul(v2, &p, 0);
				if (errno !=0 || *p != '\0' ) {
					fprintf(stderr, "Invalid BATCH_SIZE.\n");
					return 1;
				}
				batch_size_set = 1;
			}
			v2 = ini.GetValue(default_section, "I2C_SLEEP");
			if (v2 != NULL) {
				auto v3 = strtoul(v2, &p, 0);
				if (errno !=0 || *p != '\0' ) {
					fprintf(stderr, "Invalid I2C SLEEP\n");
					return 1;
				}
				i2c_sleep = chronos::milliseconds(v3);
			}
			v2 = ini.GetValue(default_section, "EMISSIVITY");
			if (v2 != NULL) {
				auto v3 = strtof(v2, &p);
				if (errno !=0 || *p != '\0' ) {
					fprintf(stderr, "Invalid I2C SLEEP\n");
					return 1;
				}
				emissivity = v3;
			}

			v2 = ini.GetValue(default_section, "DEVICE_NAME");
			v2len = v2 != NULL ? strlen(v2) : 0;
			if ( v2len ) {
				device_name = v2;
			}
			v2 = ini.GetValue(default_section, "DEVICE_ID");
			v2len = v2 != NULL ? strlen(v2) : 0;
			if ( v2len ) {
				device_id.assign(v2, v2len);
			}
			v2 = ini.GetValue(default_section, "WRITE_TO_FILE");
			v2len = v2 != NULL ? strlen(v2) : 0;
			if ( v2len ) {
				write_to_file = strtobool(v2);
			}
			v2 = ini.GetValue(default_section, "OUTFILE");
			v2len = v2 != NULL ? strlen(v2) : 0;
			if ( v2len ) {
				outfile.assign(v2, v2len);
			}
			v2 = ini.GetValue(default_section, "WRITE_TO_REDIS");
			v2len = v2 != NULL ? strlen(v2) : 0;
			if ( v2len ) {
				write_to_redis = strtobool(v2);
			}
			v2 = ini.GetValue(default_section, "REDIS_HOST");
			v2len = v2 != NULL ? strlen(v2) : 0;
			if ( v2len ) {
				redis_host = v2;
			}
			v2 = ini.GetValue(default_section, "REDIS_USER");
			v2len = v2 != NULL ? strlen(v2) : 0;
			if ( v2len ) {
				redis_user.assign(v2, v2len);
			}
			v2 = ini.GetValue(default_section, "REDIS_PASSWORD");
			v2len = v2 != NULL ? strlen(v2) : 0;
			if ( v2len ) {
				redis_password.assign(v2,v2len);
			}
			v2 = ini.GetValue(default_section, "REDIS_SENSOR_KEY");
			v2len = v2 != NULL ? strlen(v2) : 0;
			if ( v2len ) {
				redis_sensor_key.assign(v2,v2len);
			}
			v2 = ini.GetValue(default_section, "REDIS_SENSOR_INDEX");
			v2len = v2 != NULL ? strlen(v2) : 0;
			if ( v2len ) {
				redis_sensor_index.assign(v2,v2len);
			}
			v2 = ini.GetValue(default_section, "REDIS_CONFIG_KEY");
			v2len = v2 != NULL ? strlen(v2) : 0;
			if ( v2len ) {
				redis_config_key.assign(v2,v2len);
			}
			v2 = ini.GetValue(default_section, "REDIS_CONFIG_INDEX");
			v2len = v2 != NULL ? strlen(v2) : 0;
			if ( v2len ) {
				redis_config_index.assign(v2,v2len);
			}
		}

	}
	// fs::exists above sets errno=2 when the file does not exist.
	// This needs to be cleared before processing the command line because
	// strtoul does not change errno on success so the tests for 
	// overflow failed immediately.
	errno = 0;


	/*
	 * Parse the command line arguments.
	 */
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i],"-b")==0 || strcmp(argv[i],"--batch")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Batch argument without value.\n");
				return 1;
			}
			batch_size_set = 1;
			batch_size = strtoul(argv[++i], &p, 0);
			if (errno !=0 || *p != '\0') {
				fprintf(stderr, "Invalid batch size\n");
				return 1;
			}
		} else if (strcmp(argv[i],"-c")==0 || strcmp(argv[i],"--count")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Count argument without value.\n");
				return 1;
			}
			max_count = strtoul(argv[++i], &p, 0);
			if (errno !=0 || *p != '\0') {
				fprintf(stderr, "Invalid count\n");
				return 1;
			}
		} else if (strcmp(argv[i],"-d")==0 || strcmp(argv[i],"--debug")==0) {
			debug=1;			
		} else if (strcmp(argv[i],"-f")==0 || strcmp(argv[i],"--fps")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Framerate argument without value.\n");
				return 1;
			}
			fps = strtoul(argv[i+i], &p, 0);
			if (errno !=0 || *p != '\0' ) {
				fprintf(stderr, "Invalid framerate.\n");
				return 1;
			}
		} else if (strcmp(argv[i],"-o")==0 || strcmp(argv[i],"--output")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Output argument without value.\n");
				return 1;
			}
			write_to_file=1;			
			outfile.assign(argv[++i]);
		} else if (strcmp(argv[i],"-r")==0 || strcmp(argv[i],"--redis")==0) {
			write_to_redis=1;			
		} else if (strcmp(argv[i],"-s")==0 || strcmp(argv[i],"--skip")==0) {
			if (i==argc-1) {
				fprintf(stderr, "Skip frames argument without value.\n");
				return 1;
			}
			skip_frames = strtoul(argv[++i], &p, 0);
			if (errno !=0 || *p != '\0') {
				fprintf(stderr, "Invalid skip frames value.");
				return 1;
			}
		} else if (strcmp(argv[i],"--stdout")==0 || (strcmp(argv[i],"--")==0 && i==argc-1 ) ) {
			write_to_stdout=1;
		} else if (strcmp(argv[i],"--version")==0) {
			cout << argv[0] << " Version " << mlx90640_capture_VERSION_MAJOR << "." << mlx90640_capture_VERSION_MINOR << endl;
			return 0;
		} else if (strcmp(argv[i],"--help")==0) {
			cout << argv[0] << " Version " << mlx90640_capture_VERSION_MAJOR << "." << mlx90640_capture_VERSION_MINOR << endl;
			cout << "\t-f|--fps    n[int| 0, 1, 2, 4, (8), 16, 32]  - Framerate." << endl;
			cout << "\t-s|--skip   n[int| (2)]     - Skip this many frames on startup." << endl;
			cout << "\t-c|--count  n[int| (0)]     - Only record this many frames (0 => don't stop)." << endl;
			cout << "\t-b|--batch  n[int| (FPS)]   - Write data out in batches of this size." << endl;
			cout << "\t-o|--output n[string|None]  - Write data to the given filename." << endl;
			cout << "\t-r|--redis  - Write data to the given filename (Redis acces arguments must be read from a configuration file)." << endl;
			cout << "\t--|--stdout - Write data to stdout" << endl;
			return 0;
		}
	}

	/*
	 * Resolve things which depend on user given configuration values.
	 * Validate some user given input.
	 */
	if ( (fps & (fps - 1)) != 0 || !fps || fps > 64) {
		fprintf(stderr, "Invalid framerate.  Valid values are 1, 2, 4, 8, 16, 32, 64.\n");
		return 1;
	}
	if (!batch_size_set) {
		batch_size = fps;
	}
	if (max_count && batch_size > max_count) {
		batch_size = max_count;
	}

	auto frame_time = chronos::microseconds(1000000/fps + OFFSET_MICROS);


	if (debug) {
		string tmpcfg;
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.SetValue("section", "key", "newvalue");
		ini.SetValue(default_section, "BATCH_SIZE", std::to_string(batch_size).c_str());
		ini.SetValue(default_section, "FPS", std::to_string(fps).c_str());
		ini.SetValue(default_section, "DEVICE_ID", device_id.c_str());
		ini.SetValue(default_section, "DEVICE_NAME", device_name.c_str());
		ini.SetValue(default_section, "EMISSIVITY", std::to_string(emissivity).c_str());
		ini.SetValue(default_section, "I2C_SLEEP", std::to_string(i2c_sleep.count()).c_str());
		ini.SetValue(default_section, "MAX_COUNT", std::to_string(max_count).c_str());
		ini.SetValue(default_section, "OUTFILE", outfile.c_str());
		ini.SetValue(default_section, "REDIS_CONFIG_KEY", redis_config_key.c_str());
		ini.SetValue(default_section, "REDIS_CONFIG_INDEX", redis_config_index.c_str());
		ini.SetValue(default_section, "REDIS_HOST", redis_host.c_str());
		ini.SetValue(default_section, "REDIS_PASSWORD", redis_password.c_str());
		ini.SetValue(default_section, "REDIS_SENSOR_KEY", redis_sensor_key.c_str());
		ini.SetValue(default_section, "REDIS_SENSOR_INDEX", redis_sensor_index.c_str());
		ini.SetValue(default_section, "REDIS_USER", redis_user.c_str());
		ini.SetValue(default_section, "SKIP_FRAMES", std::to_string(skip_frames).c_str());
		ini.SetValue(default_section, "WRITE_TO_FILE", std::to_string(write_to_file).c_str());
		ini.SetValue(default_section, "WRITE_TO_REDIS", std::to_string(write_to_redis).c_str());
		ini.Save(tmpcfg);
		cout << tmpcfg << endl;
		cout << "*****" << endl
			<< "frame_time=" << frame_time.count()  << endl
			<< "write_to_stdout=" << write_to_stdout
			<<endl;
	}

	mlx90640::Mlx90640Frame pframes[batch_size];

	/* Build the sensor_identifier once.
	 */
	std::string sensor_identifier(hostname);
	sensor_identifier.append(":");
	sensor_identifier.append(device_name);
	sensor_identifier.append(":");
	sensor_identifier.append(device_id);

	/* Build the constant parts of the configuration string once.
	 */
	stringstream cfgss;
	cfgss << ",SENSOR="<<sensor_identifier << ",FPS="<< fps << "FRAME_TIME="<< frame_time.count() << ",DATAORDER=LR,BATCH_SIZE=" << batch_size;
	const std::string config(cfgss.str());

	/*
	 * Setup the camera
	 */
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
			fprintf(stderr, "Unsupported framerate: %ld\n", fps);
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

	cout << "count=" << count << "/" << max_count << endl;
	// Use the difference between system_clock epoch (UNIX) and steady_clock epoch
	// (unspecified) to get a reference for converting steady_clock timestamps 
	// (we cannot have backsteps in time) to a nominal unix-epoch timestamp.
	//
	const auto system_timestamp=chronos::system_clock::now();
	const auto steady_timestamp=chronos::steady_clock::now();
	// auto cfgts = chronos::duration_cast<chronos::nanoseconds>( chronos::system_clock::now().time_since_epoch() ).count();
	const auto reference_epoch_ns = (chronos::duration_cast<chronos::nanoseconds>( system_timestamp.time_since_epoch() ) - chronos::duration_cast<chronos::nanoseconds>( steady_timestamp.time_since_epoch())).count();
	ostringstream ss;
	while (!max_count || count < max_count){
		unsigned long batch_count=0;
		cout << "count=" << count << "/" << max_count << endl;
		while(batch_count < batch_size && ( !max_count || count <= max_count)) {
			cout << "batch_count=" << batch_count << "/" << batch_size << endl;
			auto start = chronos::steady_clock::now();
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
				this_thread::sleep_for(chronos::microseconds(frame_time - chronos::duration_cast<chronos::microseconds>(chronos::steady_clock::now() - start)));
				continue;
			}

			MLX90640_InterpolateOutliers(frame, eeMLX90640);
			eTa = MLX90640_GetTa(frame, &mlx90640); // Sensor ambient temprature
			MLX90640_CalculateTo(frame, &mlx90640, emissivity, eTa, mlx90640To); //calculate temprature of all pixels, base on emissivity of object

			uint64_t ts = (uint64_t)( chronos::duration_cast<chronos::nanoseconds>( start.time_since_epoch() ).count() + reference_epoch_ns );

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
			this_thread::sleep_for(chronos::microseconds(frame_time - chronos::duration_cast<chronos::microseconds>(chronos::steady_clock::now() - start)));
			++batch_count;
			++count;
			// cout << "batch_count/batch_size" << batch_count << "/" << batch_size << endl;
			// cout << "count/max_count" << count << "/" << max_count << endl;
		}
		if (write_to_stdout || write_to_file) {
			ss.clear();
			ss.str(string());
			for(unsigned long i=0; i < batch_count; ++i) {
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
