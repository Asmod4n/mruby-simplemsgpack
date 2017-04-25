MRuby::Build.new do |conf|
  conf.cc.defines << 'MRB_USE_ETEXT_EDATA'
  toolchain :gcc
  enable_debug
  conf.enable_test
  conf.gembox 'full-core'
  conf.gem File.expand_path(File.dirname(__FILE__))
end
