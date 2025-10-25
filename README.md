# Sample crate

### Dependencies
  - rtmidi
  - SDL
  - sfizz

```sh
gt clone --recursive https://github.com/sfztools/sfizz.git
cd sfizz
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
sudo make install
sudo ldconfig
```

### Compile

```sh
mkdir build
cd build 
cmake ..
cmake --build .
```
