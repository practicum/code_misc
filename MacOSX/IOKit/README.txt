build command:

g++-4.0 -o scons-out/HelperForKeyboardReaderIOKit_.object -c -isystem$BOOST/include/boost-1_49/  -isysroot/Developer/SDKs/MacOSX10.5.sdk -mmacosx-version-min=10.5  -arch i386 -g -O0   -DBOOST_PREPROC_FLAG=1490 -D__DARWIN__  -D_FILE_OFFSET_BITS=64  -D_LARGE_FILES  -DMAC_OS_X_VERSION_MIN_REQUIRED=1050  -DMACOSX_DEPLOYMENT_TARGET=10.5 -DOS_MACOSX=OS_MACOSX  -D__DEBUG__   -D_DEBUG   HelperForKeyboardReaderIOKit.cpp
