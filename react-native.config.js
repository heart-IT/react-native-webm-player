module.exports = {
  dependency: {
    platforms: {
      android: {
        componentDescriptors: ['WebmVideoViewComponentDescriptor'],
        cmakeListsPath: '../src/main/jni/CMakeLists.txt'
      }
    }
  }
}
