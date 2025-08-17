MRuby::Build.new do |conf|
  toolchain :gcc
  enable_debug
  conf.enable_sanitizer "address,undefined"
  conf.cc.flags << '-fno-omit-frame-pointer'
  conf.enable_debug
  conf.enable_test
  conf.gembox 'full-core'
  conf.gem File.expand_path(File.dirname(__FILE__))
end
