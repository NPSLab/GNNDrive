import os
from torch.utils.cpp_extension import load

dir_path = os.path.dirname(os.path.realpath(__file__))

sample = load(name='sample', sources=[os.path.join(dir_path, 'sample.cpp')], extra_cflags=['-fopenmp', '-O2'], extra_ldflags=['-lgomp','-lrt'])
gather = load(name='gather', sources=[os.path.join(dir_path, 'gather.cpp')], extra_cflags=['-fopenmp', '-O2'], extra_ldflags=['-lgomp','-lrt'])
mt_load = load(name='mt_load', sources=[os.path.join(dir_path, 'mt_load.cpp')], extra_cflags=['-fopenmp', '-O2'], extra_ldflags=['-lgomp','-lrt'])
update = load(name='update', sources=[os.path.join(dir_path, 'update.cpp')], extra_cflags=['-fopenmp', '-O2'], extra_ldflags=['-lgomp','-lrt'])
free = load(name='free', sources=[os.path.join(dir_path, 'free.cpp')], extra_cflags=['-O2'])
io_uring_support = load(name='io_uring_support', sources=[os.path.join(dir_path, 'io_uring_support.cpp')], extra_cflags=['-O2'])

def check_io_uring_support():
    import os
    if not os.path.exists('/usr/include/linux/io_uring.h'):
        return False
    else:
        return io_uring_support.io_uring_support()

cuda_path = '/usr/local/cuda'
cuda_include = os.path.join(cuda_path, 'include')
cuda_lib = os.path.join(cuda_path, 'lib64')

if check_io_uring_support():
    print("io_uring is supported, will use io_uring offloading")
    offload = load(name='offload', sources=[os.path.join(dir_path, 'offload.cpp')], 
               extra_cflags=['-fopenmp', '-g', '-lrt', '-I', cuda_include, '-L', cuda_lib], 
               extra_ldflags=['-lgomp', '-luring', '-lcuda'])
else:
    print("io_uring is not supported, will use libaio instead")
    offload = load(name='offload', sources=[os.path.join(dir_path, 'offload_libaio.cpp')], 
               extra_cflags=['-fopenmp', '-g', '-lrt', '-I', cuda_include, '-L', cuda_lib], 
               extra_ldflags=['-lgomp', '-laio', '-lcuda'])

offloadCPU = load(name='offloadCPU', sources=[os.path.join(dir_path, 'offload_share_cpu.cpp')], 
               extra_cflags=['-fopenmp', '-g'], 
               extra_ldflags=['-lgomp', '-luring', '-lrt'])

offloadGPU = load(name='offloadGPU', sources=[os.path.join(dir_path, 'offload_share_gpu.cpp')], 
               extra_cflags=['-fopenmp', '-g', '-lrt', '-I', cuda_include, '-L', cuda_lib], 
               extra_ldflags=['-lgomp', '-luring', '-lcuda'])