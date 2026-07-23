#include "interp.h"
#include "native.h"
#include "heap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

// ================= Helpers =================
static void print_int(int32_t v) {
    char buf[32]; int len = 0;
    if (v < 0) { buf[len++] = '-'; v = -v; }
    char tmp[32]; int t = 0;
    do { tmp[t++] = '0' + v % 10; v /= 10; } while (v);
    while (t) buf[len++] = tmp[--t];
    buf[len++] = '\n';
    fwrite(buf, 1, len, stdout);
}

static void print_jstr(Object* o) {
    if (o) {
        const std::string& s = o->hack_str;
        fwrite(s.c_str(), 1, s.size(), stdout);
        fwrite("\n", 1, 1, stdout);
    } else {
        fwrite("null\n", 1, 5, stdout);
    }
}

static const char* val_str(Value& v) {
    return v.obj ? v.obj->hack_str.c_str() : "";
}

// ================= Mem: malloc/free handle table =================
static std::vector<void*> g_mem_ptrs;
static std::vector<size_t> g_mem_sizes;

static int mem_alloc_handle(size_t size) {
    void* p = std::malloc(size);
    if (!p) return -1;
    for (size_t i = 0; i < g_mem_ptrs.size(); i++) {
        if (!g_mem_ptrs[i]) {
            g_mem_ptrs[i] = p;
            g_mem_sizes[i] = size;
            return (int)i + 1;
        }
    }
    g_mem_ptrs.push_back(p);
    g_mem_sizes.push_back(size);
    return (int)g_mem_ptrs.size();
}

static void* mem_resolve(int h, int off, int need) {
    if (h <= 0 || (size_t)h > g_mem_ptrs.size()) return nullptr;
    void* p = g_mem_ptrs[h - 1];
    size_t sz = g_mem_sizes[h - 1];
    if (!p || off < 0 || (size_t)(off + need) > sz) return nullptr;
    return (char*)p + off;
}

// ================= Thread stub (single-threaded host) =================
struct ThreadBoot {
    char* classpath;
    char* class_name;
    int arg;
};

static char* dup_cstr(const char* s) {
    if (!s) return nullptr;
    size_t n = strlen(s);
    char* p = (char*)std::malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static void jvm_thread_entry(void* p) {
    ThreadBoot* b = (ThreadBoot*)p;
    int code = 0;

    VM vm;
    vm.classpath = b->classpath ? b->classpath : "";
    register_natives(vm);

    ClassFile* cf = load_class(vm, b->classpath, b->class_name);
    if (!cf) {
        fprintf(stderr, "[jvm-thread] class not found: %s\n", b->class_name);
        code = 1;
    } else {
        Method* m = cf->find_method("run", "(I)V");
        if (!m || !m->is_static()) {
            fprintf(stderr, "[jvm-thread] %s needs: public static void run(int arg)\n", b->class_name);
            code = 1;
        } else {
            std::vector<Value> args;
            args.push_back(Value::fromInt(b->arg));
            vm.invoke(m, cf, args);
            if (vm.exception_obj) code = 1;
        }
    }

    free(b->classpath);
    free(b->class_name);
    free(b);
    // In single-threaded host, just return; no thread_exit()
}

// ================= Native registration =================
void register_natives(VM& vm) {
    auto println_string = [](VM& vm, std::vector<Value>& args)->Value {
        print_jstr(args[0].obj); return Value();
    };

    // Legacy compat natives
    vm.natives["print(I)V"] = [](VM& vm, std::vector<Value>& args)->Value {
        print_int(args[0].asInt()); return Value();
    };
    vm.natives["java/io/PrintStream.println(I)V"] = [](VM& vm, std::vector<Value>& args)->Value {
        print_int(args[0].asInt()); return Value();
    };
    vm.natives["java/io/PrintStream.println(Ljava/lang/String;)V"] = println_string;
    vm.natives["java/lang/System.exit(I)V"] = [](VM& vm, std::vector<Value>& args)->Value {
        exit(args[0].asInt());
        return Value();
    };
    vm.natives["java/lang/Object.<init>()V"] = [](VM& vm, std::vector<Value>& args)->Value {
        return Value();
    };
    vm.natives["printString(Ljava/lang/String;)V"] = println_string;
    vm.natives["println(Ljava/lang/String;)V"] = println_string;

    // Rmiku$IO
    vm.natives["Rmiku$IO.printInt(I)V"] = [](VM& vm, std::vector<Value>& args)->Value {
        print_int(args[0].asInt()); return Value();
    };
    vm.natives["Rmiku$IO.printStr(Ljava/lang/String;)V"] = println_string;

    vm.natives["Rmiku$IO.open(Ljava/lang/String;I)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        return Value::fromInt(open(val_str(args[0]), args[1].asInt()));
    };
    vm.natives["Rmiku$IO.create(Ljava/lang/String;)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        return Value::fromInt(open(val_str(args[0]), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    };
    vm.natives["Rmiku$IO.close(I)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        return Value::fromInt(close(args[0].asInt()));
    };
    vm.natives["Rmiku$IO.read(I[B)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        Object* arr = args[1].obj;
        if (!arr || arr->array_length <= 0) return Value::fromInt(-1);
        char tmp[1024];
        int want = arr->array_length < 1024 ? arr->array_length : 1024;
        ssize_t n = read(args[0].asInt(), tmp, (size_t)want);
        if (n <= 0) return Value::fromInt((int32_t)n);
        for (int i = 0; i < n; i++) arr->fields[i] = Value::fromInt((int32_t)(int8_t)tmp[i]);
        return Value::fromInt((int32_t)n);
    };
    vm.natives["Rmiku$IO.write(I[BI)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        Object* arr = args[1].obj;
        if (!arr) return Value::fromInt(-1);
        int len = args[2].asInt();
        if (len > arr->array_length) len = arr->array_length;
        char tmp[1024];
        int done = 0;
        while (done < len) {
            int chunk = len - done < 1024 ? len - done : 1024;
            for (int i = 0; i < chunk; i++) tmp[i] = (char)arr->fields[done + i].asInt();
            ssize_t n = write(args[0].asInt(), tmp, (size_t)chunk);
            if (n <= 0) return Value::fromInt(done > 0 ? done : (int32_t)n);
            done += (int)n;
        }
        return Value::fromInt(done);
    };
    vm.natives["Rmiku$IO.writeStr(ILjava/lang/String;)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        if (!args[1].obj) return Value::fromInt(-1);
        const std::string& s = args[1].obj->hack_str;
        return Value::fromInt((int)write(args[0].asInt(), s.c_str(), s.size()));
    };
    vm.natives["Rmiku$IO.readAll(Ljava/lang/String;)Ljava/lang/String;"] = [](VM& vm, std::vector<Value>& args)->Value {
        int fd = open(val_str(args[0]), O_RDONLY);
        if (fd < 0) return Value::fromRef(nullptr);
        std::string out;
        char tmp[512];
        ssize_t n;
        while ((n = read(fd, tmp, 512)) > 0)
            for (int i = 0; i < n; i++) out.push_back(tmp[i]);
        close(fd);
        return Value::fromRef(vm.heap.alloc_string(out.c_str()));
    };
    vm.natives["Rmiku$IO.writeAll(Ljava/lang/String;Ljava/lang/String;)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        int fd = open(val_str(args[0]), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return Value::fromInt(-1);
        int total = 0;
        if (args[1].obj) {
            const std::string& s = args[1].obj->hack_str;
            total = (int)write(fd, s.c_str(), s.size());
        }
        close(fd);
        return Value::fromInt(total);
    };
    vm.natives["Rmiku$IO.readChar()I"] = [](VM& vm, std::vector<Value>& args)->Value {
        char c;
        return Value::fromInt(read(0, &c, 1) == 1 ? (int32_t)(uint8_t)c : -1);
    };
    vm.natives["Rmiku$IO.readLine()Ljava/lang/String;"] = [](VM& vm, std::vector<Value>& args)->Value {
        std::string out;
        char c;
        while (read(0, &c, 1) == 1) {
            if (c == '\n') break;
            out.push_back(c);
        }
        return Value::fromRef(vm.heap.alloc_string(out.c_str()));
    };

    // Rmiku$Mem
    vm.natives["Rmiku$Mem.malloc(I)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        int size = args[0].asInt();
        if (size <= 0) return Value::fromInt(-1);
        return Value::fromInt(mem_alloc_handle((size_t)size));
    };
    vm.natives["Rmiku$Mem.free(I)V"] = [](VM& vm, std::vector<Value>& args)->Value {
        int h = args[0].asInt();
        if (h <= 0 || (size_t)h > g_mem_ptrs.size()) return Value();
        void* p = g_mem_ptrs[h - 1];
        g_mem_ptrs[h - 1] = nullptr;
        g_mem_sizes[h - 1] = 0;
        std::free(p);
        return Value();
    };
    vm.natives["Rmiku$Mem.load8(II)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        void* p = mem_resolve(args[0].asInt(), args[1].asInt(), 1);
        if (!p) return Value::fromInt(-1);
        return Value::fromInt((int32_t)*(uint8_t*)p);
    };
    vm.natives["Rmiku$Mem.store8(III)V"] = [](VM& vm, std::vector<Value>& args)->Value {
        void* p = mem_resolve(args[0].asInt(), args[1].asInt(), 1);
        if (p) *(uint8_t*)p = (uint8_t)args[2].asInt();
        return Value();
    };
    vm.natives["Rmiku$Mem.load32(II)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        uint8_t* p = (uint8_t*)mem_resolve(args[0].asInt(), args[1].asInt(), 4);
        if (!p) return Value::fromInt(-1);
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        return Value::fromInt((int32_t)v);
    };
    vm.natives["Rmiku$Mem.store32(III)V"] = [](VM& vm, std::vector<Value>& args)->Value {
        uint8_t* p = (uint8_t*)mem_resolve(args[0].asInt(), args[1].asInt(), 4);
        if (p) {
            uint32_t v = (uint32_t)args[2].asInt();
            p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
            p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
        }
        return Value();
    };

    // Rmiku$Proc
    vm.natives["Rmiku$Proc.fork()I"] = [](VM& vm, std::vector<Value>& args)->Value {
        return Value::fromInt((int32_t)fork());
    };
    vm.natives["Rmiku$Proc.waitpid(I)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        int st = 0;
        pid_t r = waitpid(args[0].asInt(), &st, 0);
        if (r < 0) return Value::fromInt(-1);
        return Value::fromInt((int32_t)WEXITSTATUS(st));
    };
    vm.natives["Rmiku$Proc.getpid()I"] = [](VM& vm, std::vector<Value>& args)->Value {
        return Value::fromInt((int32_t)getpid());
    };
    vm.natives["Rmiku$Proc.sleep(I)V"] = [](VM& vm, std::vector<Value>& args)->Value {
        sleep((unsigned int)args[0].asInt()); return Value();
    };
    vm.natives["Rmiku$Proc.yield()V"] = [](VM& vm, std::vector<Value>& args)->Value {
        // No-op on single-threaded host
        return Value();
    };
    vm.natives["Rmiku$Proc.exit(I)V"] = [](VM& vm, std::vector<Value>& args)->Value {
        exit(args[0].asInt());
        return Value();
    };

    // Rmiku$Thread (single-threaded stub)
    vm.natives["Rmiku$Thread.spawn(Ljava/lang/String;I)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        ThreadBoot* b = (ThreadBoot*)std::malloc(sizeof(ThreadBoot));
        if (!b) return Value::fromInt(-1);
        b->classpath = vm.classpath.empty() ? nullptr : dup_cstr(vm.classpath.c_str());
        b->class_name = dup_cstr(val_str(args[0]));
        b->arg = args[1].asInt();
        // Single-threaded: run synchronously
        jvm_thread_entry(b);
        return Value::fromInt(0);
    };
    vm.natives["Rmiku$Thread.join(I)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        // Single-threaded: already finished
        return Value::fromInt(0);
    };

    // Rmiku$Net (Berkeley sockets)
    vm.natives["Rmiku$Net.udpSocket()I"] = [](VM& vm, std::vector<Value>& args)->Value {
        return Value::fromInt(socket(AF_INET, SOCK_DGRAM, 0));
    };
    vm.natives["Rmiku$Net.tcpSocket()I"] = [](VM& vm, std::vector<Value>& args)->Value {
        return Value::fromInt(socket(AF_INET, SOCK_STREAM, 0));
    };
    vm.natives["Rmiku$Net.bind(II)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = INADDR_ANY;
        a.sin_port = htons((unsigned short)args[1].asInt());
        return Value::fromInt(bind(args[0].asInt(), (struct sockaddr*)&a, sizeof(a)));
    };
    vm.natives["Rmiku$Net.connect(III)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl((unsigned int)args[1].asInt());
        a.sin_port = htons((unsigned short)args[2].asInt());
        return Value::fromInt(connect(args[0].asInt(), (struct sockaddr*)&a, sizeof(a)));
    };
    vm.natives["Rmiku$Net.send(ILjava/lang/String;)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        if (!args[1].obj) return Value::fromInt(-1);
        const std::string& s = args[1].obj->hack_str;
        return Value::fromInt((int)send(args[0].asInt(), s.c_str(), s.size(), 0));
    };
    vm.natives["Rmiku$Net.sendTo(ILjava/lang/String;II)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        if (!args[1].obj) return Value::fromInt(-1);
        const std::string& s = args[1].obj->hack_str;
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl((unsigned int)args[2].asInt());
        a.sin_port = htons((unsigned short)args[3].asInt());
        return Value::fromInt((int)sendto(args[0].asInt(), s.c_str(), s.size(), 0,
                                          (struct sockaddr*)&a, sizeof(a)));
    };
    vm.natives["Rmiku$Net.recv(I[B)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        Object* arr = args[1].obj;
        if (!arr || arr->array_length <= 0) return Value::fromInt(-1);
        char tmp[1500];
        int want = arr->array_length < 1500 ? arr->array_length : 1500;
        int n = (int)recv(args[0].asInt(), tmp, want, 0);
        if (n <= 0) return Value::fromInt(n);
        for (int i = 0; i < n; i++) arr->fields[i] = Value::fromInt((int32_t)(int8_t)tmp[i]);
        return Value::fromInt(n);
    };
    vm.natives["Rmiku$Net.recvFrom(I[B)I"] = [](VM& vm, std::vector<Value>& args)->Value {
        Object* arr = args[1].obj;
        if (!arr || arr->array_length <= 0) return Value::fromInt(-1);
        char tmp[1500];
        int want = arr->array_length < 1500 ? arr->array_length : 1500;
        int n = (int)recvfrom(args[0].asInt(), tmp, want, 0, nullptr, nullptr);
        if (n <= 0) return Value::fromInt(n);
        for (int i = 0; i < n; i++) arr->fields[i] = Value::fromInt((int32_t)(int8_t)tmp[i]);
        return Value::fromInt(n);
    };
}

Value call_native(VM& vm, const std::string& cls, const std::string& name,
                  const std::string& desc, std::vector<Value>& args) {
    std::string key = cls + "." + name + desc;
    auto it = vm.natives.find(key);
    if (it != vm.natives.end()) return it->second(vm, args);
    key = name + desc;
    it = vm.natives.find(key);
    if (it != vm.natives.end()) return it->second(vm, args);
    fprintf(stderr, "Unsatisfied native: %s.%s%s\n", cls.c_str(), name.c_str(), desc.c_str());
    exit(1);
}