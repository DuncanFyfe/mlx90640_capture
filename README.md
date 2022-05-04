# MLX90640 Thermal Camera Data Capture

## Copyright

Copyright 2022 Duncan Fyfe
Licenced under Apache License 2.0.
See the [Licence File](https://github.com/DuncanFyfe/mlx90640_capture/blob/93a9fd8f3e41666e31f22dee7868ddbb59e20e77/LICENSE) for details.

## Contents

*  Source for `mlx90640_capture` a program which captures images from an MLX90640 Thermal Camera
*  Ansible playbook to install hardware dependencies
*  Ansible playbook to install redis dependencies

## Installation

*  CMake
*  make
*  make install

## Notes 

*  For %REASONS I usually build from github archives rather than github directly.  I place these in a directoy "external" beside the playbooks.
*  I have more than one sensor hanging off the same i2c bus so I set the baudrate to 400000.
*  The /melexis-fir/ software is half-baked. You will need to experiment and explore to find out what works and what doesn't.
*  The /pimoroni/ driver `./functions/MLX90640_API.cpp:MLX90640_GetFrameData` tries to read data from the camera.  Under repeat failure (`"cnt > 4 error"`) it stops trying and returns what it has without error (although it does print the above message).  The code contains a commented out `return -8` which would be useful for programatic handling of these events.  This error typically occurs when the I2C bus isn't running fast enough to transfer the requested frames per second. With `i2c1_baudrate=400000` I can reliably acquire 8 FPS but not 16 or more.


## Reference Material

### Product:
*  [MLX90640 Breakout Unit](https://shop.pimoroni.com/products/mlx90640-thermal-camera-breakout?variant=12536948654163)
*  [MLX90640 Thermal Camera](https://www.melexis.com/en/product/mlx90640/far-infrared-thermal-sensor-array)

### Software:
 [Pimoroni MLX90640 Library](https://github.com/pimoroni/mlx90640-library)
 [Melexis I2C Library](https://github.com/melexis-fir/mlx90640-driver-devicetree-py)
 [Melexis MLX90640 Library](https://github.com/melexis-fir/mlx90640-driver-py)
 [Pip installable version of the Melexis MLX90640 Library](https://pypi.org/project/mlx90640-driver/)
 [Alternative I2C library](http://www.airspayce.com/mikem/bcm2835/)
 [Sparkfun Non-Raspberry Pi Example](https://github.com/sparkfun/SparkFun_MLX90640_Arduino_Example)
