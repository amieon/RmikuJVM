#include "types.h"
#include "classfile.h"
#include "heap.h"
#include "interp.h"
#include "native.h"

#include "aot.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

static const int MAX_CLASSES = 32;
static ClassFile g_class_storage[MAX_CLASSES];
static int g_class_count = 0;

static bool read_file(const char* path, uint8_t* buf, int* out_len) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    int len = 0;
    while (len < 65536) {
        size_t n = std::fread(buf + len, 1, 65536 - len, f);
        if (n == 0) break;
        len += (int)n;
    }
    std::fclose(f);
    *out_len = len;
    return true;
}

static void build_path(char* out, const char* classpath, const char* classname) {
    int i = 0;
    if (classpath) {
        while (*classpath) out[i++] = *classpath++;
        out[i++] = '/';
    }
    while (*classname) {
        char c = *classname++;
        out[i++] = (c == '.') ? '/' : c;
    }
    out[i++] = '.'; out[i++] = 'c'; out[i++] = 'l'; out[i++] = 'a';
    out[i++] = 's'; out[i++] = 's'; out[i] = '\0';
}

static std::mutex g_load_lock;

static ClassFile* load_class_locked(VM& vm, const char* classpath, const char* classname) {
    std::string name = classname;
    if (vm.classes.count(name)) return vm.classes[name];
    if (g_class_count >= MAX_CLASSES) {
        std::printf("Too many classes\n");
        return nullptr;
    }

    char path[256];
    build_path(path, classpath, classname);

    static uint8_t buf[65536];
    int len = 0;
    if (!read_file(path, buf, &len)) {
        std::printf("Class not found: %s (tried %s)\n", classname, path);
        return nullptr;
    }

    ClassFile* cf = &g_class_storage[g_class_count++];
    *cf = parse_class(buf);
    vm.classes[cf->cp_class_name(cf->this_class)] = cf;

    if (cf->super_class != 0) {
        std::string sname = cf->cp_class_name(cf->super_class);
        if (sname == "java/lang/Object") {
            if (!vm.classes.count(sname)) {
                if (g_class_count >= MAX_CLASSES) {
                    std::printf("Too many classes\n");
                    return nullptr;
                }
                ClassFile* obj = &g_class_storage[g_class_count++];
                *obj = ClassFile();
                obj->cp.resize(3);
                obj->cp[1].tag = 1; obj->cp[1].str = "java/lang/Object";
                obj->cp[2].tag = 7; obj->cp[2].u.class_name_idx = 1;
                obj->this_class = 2;
                Method init;
                init.flags = 0x0100;
                init.name = "<init>";
                init.desc = "()V";
                obj->methods.push_back(init);
                vm.classes[sname] = obj;
            }
            cf->super = vm.classes[sname];
        } else {
            cf->super = load_class_locked(vm, classpath, sname.c_str());
        }
    }
    aot_compile_class(cf);
    return cf;
}

ClassFile* load_class(VM& vm, const char* classpath, const char* classname) {
    std::lock_guard<std::mutex> lock(g_load_lock);
    return load_class_locked(vm, classpath, classname);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: jvm [classpath] <MainClass>\n");
        return 1;
    }

    const char* classpath = nullptr;
    char name_buf[256];
    char dir_buf[256];
    if (argc > 2) {
        classpath = argv[1];
        int i = 0;
        while (argv[2][i] && i < 255) { name_buf[i] = argv[2][i]; i++; }
        name_buf[i] = '\0';
    } else {
        int i = 0;
        while (argv[1][i] && i < 255) { name_buf[i] = argv[1][i]; i++; }
        name_buf[i] = '\0';
        int len = i;
        if (len > 6 && name_buf[len-6] == '.' && name_buf[len-5] == 'c' &&
            name_buf[len-4] == 'l' && name_buf[len-3] == 'a' &&
            name_buf[len-2] == 's' && name_buf[len-1] == 's') {
            name_buf[len-6] = '\0';
        }
        int slash = -1;
        for (int j = 0; name_buf[j]; j++) {
            if (name_buf[j] == '/') slash = j;
        }
        if (slash >= 0) {
            for (int j = 0; j < slash; j++) dir_buf[j] = name_buf[j];
            dir_buf[slash] = '\0';
            int k = 0;
            for (int j = slash + 1; name_buf[j]; j++) name_buf[k++] = name_buf[j];
            name_buf[k] = '\0';
            classpath = dir_buf;
        }
    }
    const char* main_name = name_buf;

    VM vm;
    vm.classpath = classpath ? classpath : "";
    ClassFile* main_cf = load_class(vm, classpath, main_name);
    if (!main_cf) {
        std::printf("Cannot load main class: %s\n", main_name);
        return 1;
    }
    vm.main_class = main_cf;
    register_natives(vm);

    Method* m = main_cf->find_method("main", "([Ljava/lang/String;)V");
    if (!m) { std::printf("no main\n"); return 1; }

    Object* args_arr = vm.heap.alloc_array(0, T_REF);
    std::vector<Value> args;
    args.push_back(Value::fromRef(args_arr));
    vm.invoke(m, main_cf, args);

    if (vm.exception_obj) {
        std::printf("Uncaught exception\n");
        return 1;
    }
    return 0;
}