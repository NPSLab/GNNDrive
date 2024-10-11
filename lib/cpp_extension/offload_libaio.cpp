#include <stdio.h>
#include <ATen/ATen.h>
#include <torch/extension.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <list>
#include <error.h>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <sys/uio.h>
#include <omp.h>
#include <atomic>
#include <libaio.h>
#include <cuda_runtime.h>
#include <cstring>
#include <cstdlib>

#define ALIGNMENT 512
#define ASYNC_ENYRY_NUM 80
#define EVENT_BUFFER_SIZE 4


 enum class AsyncType {
    CPU,
    GPU,
    GDS,
    None
};

typedef struct map_info_s
{
    int64_t index;
    int32_t ref;
    int32_t valid;
    struct iocb *iocb;
} map_info;

class Offloader
{
public:
    Offloader(const std::string &filename, const int64_t node_num, 
        const int64_t dim, const int64_t buffer_size, 
        const std::string &type = "cpu", int device_id = 0, int stage_size = 0);
    ~Offloader();

    torch::Tensor get_tensor();

    torch::Tensor async_load(torch::Tensor &idx, int t_id = 0, int t_total = 1);

    void release(torch::Tensor &idx);

private:
    AsyncType async_type;

    const std::string filename;
    int fd;

    torch::Tensor feature_tensor;
    float *cache_data;
    int64_t feature_dim;
    int64_t cache_size;
    std::vector<int64_t> back_index;
    size_t mem_size = 0;

    std::mutex update_mutex;
    int64_t node_size;
    std::vector<map_info> map_table;
    // free table
    int group_size;
    int64_t free_index_size;
    std::list<int64_t> free_lru_list;
    std::unordered_map<int64_t, std::list<int64_t>::iterator> free_map_table;

    int64_t get_free_index() {
        if (this->free_lru_list.empty()) {
            return -1;
        }

        int64_t index = this->free_lru_list.front();
        this->free_lru_list.pop_front();
        this->free_map_table.erase(index);
        int64_t orignal_key = this->back_index[index];
        if (orignal_key >= 0)
        {
            this->map_table[orignal_key].valid = 0;
        }
        return index;
    }

    void put_free_index(int64_t index) {
        this->free_lru_list.push_back(index);
        auto it = this->free_lru_list.end();
        it--;
        this->free_map_table.insert({index, it});
    }

    bool reuse_free_index(int64_t index){
        auto it = this->free_map_table.find(index);
        if (it != this->free_map_table.end()) {
            this->free_lru_list.erase(it->second);
            this->free_map_table.erase(it);
            return true;
        }
        return false;
    }
    
    void init_cpu();
    torch::Tensor cpu_async_load(torch::Tensor &idx);


    int64_t stage_size = 0;
    size_t stage_mem_size = 0;
    std::vector<int64_t> stage_map_table;

    float *device_cache;
    void init_gpu(int device_id);
    torch::Tensor gpu_async_load(torch::Tensor &idx, int t_id = 0, int t_total = 1);

    void load_callback(int key, cudaStream_t& cuda_read_stream);
    
    // torch::Tensor gds_async_load(torch::Tensor &idx);

};


Offloader::Offloader(const std::string &filename, const int64_t node_num, 
    const int64_t dim, const int64_t buffer_size, const std::string &type, int device_id, int stage_size) 
    : filename(filename), node_size(node_num), feature_dim(dim), cache_size(buffer_size), stage_size(stage_size)
{
    this->group_size = ALIGNMENT / (this->feature_dim * sizeof(float));
    if (this->group_size < 1) {
        this->group_size = 1;
    }

    this->free_index_size = this->cache_size;
    this->cache_size = this->cache_size * group_size;

    this->fd = open(filename.c_str(), O_RDONLY | O_DIRECT);
    if (this->fd < 0)
    {
        fprintf(stderr, "open file %s failed %s\n", filename.c_str(), strerror(errno));
    }

    if (strcasecmp("cpu", type.c_str()) == 0)
    {
        this->async_type = AsyncType::CPU;
        init_cpu();
    } else if (strcasecmp("gpu", type.c_str()) == 0)
    {
        this->async_type = AsyncType::GPU;
        init_gpu(device_id);
    } else if (strcasecmp("gds", type.c_str()) == 0)
    {
        this->async_type = AsyncType::GDS;
    } else {
        this->async_type = AsyncType::None;
    }

    this->map_table.resize(this->node_size);
    this->back_index.resize(this->cache_size);
    this->back_index.assign(this->cache_size, -1);

    for (int64_t i = 0; i < this->free_index_size; i++)
    {
        put_free_index(i);
    }
    printf("Offloader init done %s %ld %ld %ld %s %d %ld %llu\n", filename.c_str(), 
            this->node_size, this->feature_dim, this->cache_size, type.c_str(), device_id, this->mem_size, this->mem_size + (char *)this->cache_data);

}


void Offloader::init_cpu() 
{
    this->mem_size = this->cache_size * this->feature_dim * sizeof(float);
    if (this->mem_size % ALIGNMENT)
        this->mem_size = (this->mem_size / ALIGNMENT + 1) * ALIGNMENT;

    this->cache_data = (float *)aligned_alloc(4096, this->mem_size);

    auto options = torch::TensorOptions()
        .dtype(torch::kFloat32)
        .layout(torch::kStrided)
        .device(torch::kCPU)
        .requires_grad(false);
    
    this->feature_tensor = torch::from_blob(this->cache_data, 
            {this->cache_size, this->feature_dim}, options);

}


void Offloader::init_gpu(int device_id) 
{
    this->mem_size = this->cache_size * this->feature_dim * sizeof(float);
    if (this->mem_size % ALIGNMENT)
        this->mem_size = (this->mem_size / ALIGNMENT + 1) * ALIGNMENT;

    this->stage_mem_size = this->stage_size * this->group_size * this->feature_dim * sizeof(float);
    if (this->stage_mem_size % ALIGNMENT)
        this->stage_mem_size = (this->stage_mem_size / ALIGNMENT + 1) * ALIGNMENT;

    this->stage_map_table.resize(this->node_size);

    cudaSetDevice(device_id);

    cudaMallocHost(&this->cache_data, this->stage_mem_size);

    cudaMalloc(&this->device_cache, this->mem_size);

    auto options = torch::TensorOptions()
        .dtype(torch::kFloat32)
        .layout(torch::kStrided)
        .device(torch::kCUDA, device_id)
        .requires_grad(false);
    
    this->feature_tensor = torch::from_blob(this->device_cache, 
            {this->cache_size, this->feature_dim}, options);

}


Offloader::~Offloader()
{
    switch (this->async_type)
    {
    case AsyncType::CPU:
        if(this->cache_data){
            free(this->cache_data);
            this->cache_data = nullptr;
        }
        break;
    case AsyncType::GPU:
        if(this->cache_data){
            cudaFreeHost(this->cache_data);
            this->cache_data = nullptr;
        }
        if(this->device_cache){
            cudaFree(this->device_cache);
            this->device_cache = nullptr;
        }
        break;
    case AsyncType::GDS:
        break;
    default:
        break;
    }
    
    close(this->fd);
}

torch::Tensor Offloader::get_tensor()
{
    if (this->fd > 0)
        return this->feature_tensor;
    else
        return torch::zeros(0);
}

torch::Tensor Offloader::async_load(torch::Tensor &idx, int t_id, int t_total) 
{
    switch (this->async_type)
    {
    case AsyncType::CPU:
        return cpu_async_load(idx);
    case AsyncType::GPU:
        return gpu_async_load(idx, t_id, t_total);
    case AsyncType::GDS:
        // return gds_async_load(idx);
    default:
        fprintf(stderr, "Not support: %d\n", this->async_type);
        break;
    }
}

torch::Tensor Offloader::cpu_async_load(torch::Tensor &idx) 
{
    omp_lock_t lock;
    omp_init_lock(&lock);
    bool need_load = false;
    std::unordered_set<int64_t> need_wait;

    torch::Tensor remap_idx = torch::zeros_like(idx);
    int64_t num_idx = idx.numel();
    auto idx_data = idx.data_ptr<int64_t>();
    auto remap_data = remap_idx.data_ptr<int64_t>();

    this->update_mutex.lock();

    for (int64_t n = 0; n < num_idx; n++) {
        int64_t key = idx_data[n];
        int64_t offset = 0;
        if (this->group_size > 1) {
            offset = key % this->group_size;
            key = key / this->group_size * this->group_size;
        }
        if (key > this->map_table.size())
            printf("key %lld\n", key);
        if (this->map_table[key].valid > 0) {
            remap_data[n] = this->map_table[key].index * this->group_size + offset;
            if (this->map_table[key].ref == 0) {
                omp_set_lock(&lock);
                reuse_free_index(this->map_table[key].index);
                omp_unset_lock(&lock);
            }
        } else {
            if (this->map_table[key].ref > 0) {
                remap_data[n] = this->map_table[key].index * this->group_size + offset;
                omp_set_lock(&lock);
                need_wait.insert(key);
                omp_unset_lock(&lock);
            } else {
                remap_data[n] = -1;
                need_load = true;
            }
        }
        this->map_table[key].ref += 1;
    }

    if (need_load > 0) {
        io_context_t ctx;
        int64_t finished = 0;
        int64_t async_loading = 0;
        struct io_event events[EVENT_BUFFER_SIZE];

        memset(&ctx, 0, sizeof(ctx));
        int ret = io_setup(ASYNC_ENYRY_NUM, &ctx);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to setup io_context: %s\n", strerror(-ret));
            io_destroy(ctx);
            goto err_lock;
        }

        for (int64_t n = 0; n < num_idx; n++)
        {
            if (remap_data[n] >= 0)
                continue;

            int64_t key = idx_data[n];
            int64_t offset = 0;
            if (this->group_size > 1) {
                // loaded before
                offset = key % this->group_size;
                key = key / this->group_size * this->group_size;
                if (this->back_index[this->map_table[key].index] == key) {
                    remap_data[n] = this->map_table[key].index * this->group_size + offset;
                    continue;
                }
            }

            int64_t index = get_free_index();
            if (index < 0)
            {
                fprintf(stderr, "No free table.\n");
                io_destroy(ctx);
                goto err_lock;
            }
            remap_data[n] = index * this->group_size + offset;
            this->map_table[key].index = index;
            struct iocb *iocb_ptr = new struct iocb;
            this->map_table[key].iocb = iocb_ptr;
            this->back_index[index] = key;

            float *f_buffer;
            f_buffer = this->cache_data + index * this->group_size * this->feature_dim;
            unsigned f_nbytes = this->feature_dim * sizeof(float);
            if (f_nbytes < ALIGNMENT)
                f_nbytes = ALIGNMENT;
            uint64_t f_offset = key * this->feature_dim * sizeof(float);
            io_prep_pread(iocb_ptr, this->fd, f_buffer, f_nbytes, f_offset);
            int64_t *keyptr = new int64_t;
            *keyptr = key;
            iocb_ptr->data = reinterpret_cast<void *>(keyptr);
            ret = io_submit(ctx, 1, &iocb_ptr);
            if (ret < 0)
            {
                fprintf(stderr, "Error in io_submit: %s\n", strerror(-ret));
                delete iocb_ptr;
                delete keyptr;
                io_destroy(ctx);
                goto err_lock;
            }
            async_loading += 1;

            ret = io_getevents(ctx, 0, 1, events, nullptr);
            if (ret > 0)
            {
                int64_t cqe_key = *reinterpret_cast<int64_t *>(events[0].data);
                delete reinterpret_cast<int64_t *>(events[0].data);
                if (events[0].res < 0)
                {
                    fprintf(stderr, "Error in async operation in cpu: %s %d\n", strerror(-events[0].res), cqe_key);
                }
                this->map_table[cqe_key].valid = 1;
                delete this->map_table[cqe_key].iocb;
                finished += 1;
            }
        }
        this->update_mutex.unlock();

        while (finished < async_loading)
        {
            ret = io_getevents(ctx, 1, EVENT_BUFFER_SIZE, events, nullptr);
            if (ret < 0)
            {
                fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
                io_destroy(ctx);
                goto err;
            }
            for (int i = 0; i < ret; i++)
            {
                int64_t cqe_key = *reinterpret_cast<int64_t *>(events[i].data);
                delete reinterpret_cast<int64_t *>(events[i].data);
                if (events[i].res < 0)
                {
                    fprintf(stderr, "Error in async operation in cpu: %s %d\n", strerror(-events[i].res), cqe_key);
                }
                this->map_table[cqe_key].valid = 1;
                delete this->map_table[cqe_key].iocb;
                finished += 1;
            }
        }
        io_destroy(ctx);
    } else {
        this->update_mutex.unlock();
    }
    
    for (int64_t key : need_wait) {
        while (this->map_table[key].valid == 0) {
            std::this_thread::yield();
        }
    }
    return remap_idx;

err_lock:
    this->update_mutex.unlock();
err:
    return torch::zeros(0);
}



void Offloader::load_callback(int key, cudaStream_t& cuda_read_stream)
{
    int host_index = this->stage_map_table[key];

    int index = this->map_table[key].index;

    this->map_table[key].valid = 1;
    delete this->map_table[key].iocb;

    float *host_buffer;
    host_buffer = this->cache_data + host_index * this->group_size * this->feature_dim;
    float *dev_buffer;
    dev_buffer = this->device_cache + index * this->group_size * this->feature_dim;
    unsigned cuda_nbytes = this->feature_dim * sizeof(float);
    if (cuda_nbytes < ALIGNMENT)
        cuda_nbytes = ALIGNMENT;
    cudaMemcpyAsync(dev_buffer, host_buffer, cuda_nbytes,
                    cudaMemcpyHostToDevice, cuda_read_stream);
}

// ssd -> host mem -> gpu mem
torch::Tensor Offloader::gpu_async_load(torch::Tensor &idx, int t_id, int t_total) 
{
    omp_lock_t lock;
    omp_init_lock(&lock);
    bool need_load = false;
    std::unordered_set<int64_t> need_wait;

    torch::Tensor remap_idx = torch::zeros_like(idx);
    int64_t num_idx = idx.numel();
    auto idx_data = idx.data_ptr<int64_t>();
    auto remap_data = remap_idx.data_ptr<int64_t>();

    this->update_mutex.lock();

    for (int64_t n = 0; n < num_idx; n++) {
        int64_t key = idx_data[n];
        int64_t offset = 0;
        if (this->group_size > 1) {
            offset = key % this->group_size;
            key = key / this->group_size * this->group_size;
        }
        if (key > this->map_table.size())
            printf("key %lld\n", key);
        if (this->map_table[key].valid > 0) {
            remap_data[n] = this->map_table[key].index * this->group_size + offset;
            if (this->map_table[key].ref == 0) {
                omp_set_lock(&lock);
                reuse_free_index(this->map_table[key].index);
                omp_unset_lock(&lock);
            }
        } else {
            if (this->map_table[key].ref > 0) {
                remap_data[n] = this->map_table[key].index * this->group_size + offset;
                omp_set_lock(&lock);
                need_wait.insert(key);
                omp_unset_lock(&lock);
            } else {
                remap_data[n] = -1;
                need_load = true;
            }
        }
        this->map_table[key].ref += 1;
    }

    if (need_load > 0) {
        io_context_t ctx;
        int64_t finished = 0;
        int64_t async_loading = 0;
        struct io_event events[EVENT_BUFFER_SIZE];
        cudaStream_t read_stream;
        cudaStreamCreate(&read_stream);

        memset(&ctx, 0, sizeof(ctx));
        int ret = io_setup(ASYNC_ENYRY_NUM, &ctx);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to setup io_context: %s\n", strerror(-ret));
            io_destroy(ctx);
            goto err_lock;
        }

        for (int64_t n = 0; n < num_idx; n++)
        {
            if (remap_data[n] >= 0)
                continue;

            int64_t key = idx_data[n];
            int64_t offset = 0;
            if (this->group_size > 1) {
                // loaded before
                offset = key % this->group_size;
                key = key / this->group_size * this->group_size;
                if (this->back_index[this->map_table[key].index] == key) {
                    remap_data[n] = this->map_table[key].index * this->group_size + offset;
                    continue;
                }
            }

            int64_t host_index = this->stage_size / t_total * t_id + n;
            if (host_index >= this->stage_size)
            {
                fprintf(stderr, "No free table in host. %d %d %d\n", this->stage_size, host_index, n);
                io_destroy(ctx);
                goto err_lock;
            } else {
                this->stage_map_table[key] = host_index;
            }

            int64_t index = get_free_index();
            if (index < 0)
            {
                fprintf(stderr, "No free table in gpu.\n");
                io_destroy(ctx);
                goto err_lock;
            }
            remap_data[n] = index * this->group_size + offset;
            this->map_table[key].index = index;
            struct iocb *iocb_ptr = new struct iocb;
            this->map_table[key].iocb = iocb_ptr;        
            this->back_index[index] = key;

            float *f_buffer;
            f_buffer = this->cache_data + host_index * this->group_size * this->feature_dim;
            unsigned f_nbytes = this->feature_dim * sizeof(float);
            if (f_nbytes < ALIGNMENT)
                f_nbytes = ALIGNMENT;
            uint64_t f_offset = key * this->feature_dim * sizeof(float);
            io_prep_pread(iocb_ptr, this->fd, f_buffer, f_nbytes, f_offset);
            int64_t *keyptr = new int64_t;
            *keyptr = key;
            iocb_ptr->data = reinterpret_cast<void *>(keyptr);
            ret = io_submit(ctx, 1, &iocb_ptr);
            if (ret < 0)
            {
                fprintf(stderr, "Error in io_submit: %s\n", strerror(-ret));
                delete iocb_ptr;
                delete keyptr;
                io_destroy(ctx);
                goto err_lock;
            }
            async_loading += 1;

            ret = io_getevents(ctx, 0, 1, events, nullptr);
            if (ret > 0)
            {
                int64_t cqe_key = *reinterpret_cast<int64_t *>(events[0].data);
                delete reinterpret_cast<int64_t *>(events[0].data);
                if (events[0].res < 0)
                {
                    fprintf(stderr, "Error in async operation in gpu: %s %d\n", strerror(-events[0].res), cqe_key);
                }
                finished += 1;
                load_callback(cqe_key, read_stream);
            }
        }
        this->update_mutex.unlock();

        while (finished < async_loading)
        {
            ret = io_getevents(ctx, 1, EVENT_BUFFER_SIZE, events, nullptr);
            if (ret < 0)
            {
                fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
                io_destroy(ctx);
                goto err;
            }
            for (int i = 0; i < ret; i++)
            {
                int64_t cqe_key = *reinterpret_cast<int64_t *>(events[i].data);
                delete reinterpret_cast<int64_t *>(events[i].data);
                if (events[i].res < 0)
                {
                    fprintf(stderr, "Error in async operation in gpu: %s %d\n", strerror(-events[i].res), cqe_key);
                }
                finished += 1;
                load_callback(cqe_key, read_stream);
            }
        }
        io_destroy(ctx);

        cudaStreamSynchronize(read_stream);
        cudaStreamDestroy(read_stream);
    } else {
        this->update_mutex.unlock();
    }
    
    for (int64_t key : need_wait) {
        while (this->map_table[key].valid == 0) {
            std::this_thread::yield();
        }
    }
    return remap_idx;

err_lock:
    this->update_mutex.unlock();
err:
    return torch::zeros(0);
}


void Offloader::release(torch::Tensor &idx)
{
    omp_lock_t lock;
    omp_init_lock(&lock);

    int64_t num_idx = idx.numel();
    auto idx_data = idx.data_ptr<int64_t>();

    this->update_mutex.lock();

    for (int64_t n = 0; n < num_idx; n++) {
        int64_t key = idx_data[n];
        if (this->group_size > 1) {
            key = key / this->group_size * this->group_size;
        }
        this->map_table[key].ref -= 1;
        if (this->map_table[key].ref == 0) {
            omp_set_lock(&lock);
            put_free_index(this->map_table[key].index);
            omp_unset_lock(&lock);
        }
    }

    this->update_mutex.unlock();
}


namespace py = pybind11;

PYBIND11_MODULE(offload, m)
{
    py::class_<Offloader>(m, "Offloader")
        .def(py::init<const std::string &, const int64_t, const int64_t, const int64_t, 
             const std::string &, int, int>(),
             py::arg("filename"), py::arg("node_num"), py::arg("dim"), py::arg("buffer_size"), 
             py::arg("type"), py::arg("device_id"), py::arg("stage_size"))
        .def("async_load", &Offloader::async_load, py::arg("tensor"), py::arg("t_id"), py::arg("t_total"))
        .def("release", &Offloader::release, py::arg("tensor"))
        .def("get_tensor", &Offloader::get_tensor);
}
