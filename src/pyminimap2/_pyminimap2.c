#define _GNU_SOURCE
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <setjmp.h>
#include <string.h>

#ifdef __linux__
#include <sys/syscall.h>
#include <errno.h>
#endif

/* 
   Declare the renamed main function from minimap2.
   The macro -Dmain=mm2_main ensures minimap2 objects define mm2_main.
*/
extern int mm2_main(int argc, char *argv[]);

/*
   Global jump buffer for exit hijacking.
   This is non-reentrant, but minimap2 uses global state anyway.
*/
static jmp_buf exit_env;
static int exit_code_val = 0;

/*
   Hijack exit function.
   This function will be called instead of the standard exit()
   due to -Dexit=mm2_exit macro.
   It must be non-static to be visible to other object files.
*/
void mm2_exit(int status) {
    exit_code_val = status;
    longjmp(exit_env, 1);
}

#ifdef __linux__
// Use syscall directly to avoid glibc version dependency
// This allows the code to work on older glibc (e.g., manylinux2014 with glibc 2.17)
// while still using memfd_create on newer kernels (Linux 3.17+)
static int memfd_create_syscall(const char *name, unsigned int flags) {
#ifdef SYS_memfd_create
    return syscall(SYS_memfd_create, name, flags);
#else
    errno = ENOSYS;
    return -1;
#endif
}
#endif

static FILE* open_temp_buffer(void) {
#ifdef __linux__
    // Use syscall to create an in-memory anonymous file
    // 1 = MFD_CLOEXEC (defined as 0x0001U in linux/memfd.h)
    int fd = memfd_create_syscall("minimap2_buffer", 1);
    if (fd != -1) {
        FILE* fp = fdopen(fd, "w+");
        if (fp) return fp;
        close(fd);
    }
    // If memfd_create fails (old kernel or not supported), fall back to tmpfile
#endif
    return tmpfile();
}

static PyObject* method_main(PyObject* self, PyObject* args) {
    PyObject* input_arg;
    PyObject* py_arg_list = NULL;
    int is_new_ref = 0;

    // Expect a single argument
    if (!PyArg_ParseTuple(args, "O", &input_arg)) {
        return NULL;
    }

    if (PyList_Check(input_arg)) {
        py_arg_list = input_arg;
        is_new_ref = 0;
    } else if (PyUnicode_Check(input_arg)) {
        // Import shlex to split the string
        PyObject* shlex_module = PyImport_ImportModule("shlex");
        if (!shlex_module) return NULL;
        
        PyObject* split_func = PyObject_GetAttrString(shlex_module, "split");
        Py_DECREF(shlex_module);
        if (!split_func) return NULL;
        
        PyObject* call_args = PyTuple_Pack(1, input_arg);
        if (!call_args) {
             Py_DECREF(split_func);
             return NULL;
        }
        
        py_arg_list = PyObject_CallObject(split_func, call_args);
        Py_DECREF(split_func);
        Py_DECREF(call_args);
        
        if (!py_arg_list) return NULL;
        
        if (!PyList_Check(py_arg_list)) {
            Py_DECREF(py_arg_list);
            PyErr_SetString(PyExc_TypeError, "shlex.split did not return a list");
            return NULL;
        }
        is_new_ref = 1;
    } else {
        PyErr_SetString(PyExc_TypeError, "Argument must be a list of strings or a command string");
        return NULL;
    }

    // Convert list to argv
    // argv[0] is program name "minimap2"
    // argv[1..n] are arguments from the list
    Py_ssize_t list_len = PyList_Size(py_arg_list);
    int argc = (int)list_len + 1;
    char** argv = malloc((argc) * sizeof(char*));
    if (!argv) {
        if (is_new_ref) Py_DECREF(py_arg_list);
        return PyErr_NoMemory();
    }

    argv[0] = strdup("minimap2");
    if (!argv[0]) {
        free(argv);
        if (is_new_ref) Py_DECREF(py_arg_list);
        return PyErr_NoMemory();
    }

    for (Py_ssize_t i = 0; i < list_len; i++) {
        PyObject* item = PyList_GetItem(py_arg_list, i);
        if (!PyUnicode_Check(item)) {
            for (int k = 0; k <= i; k++) free(argv[k]);
            free(argv);
            if (is_new_ref) Py_DECREF(py_arg_list);
            PyErr_SetString(PyExc_TypeError, "List items must be strings");
            return NULL;
        }
        const char* str_utf8 = PyUnicode_AsUTF8(item);
        if (!str_utf8) {
            for (int k = 0; k <= i; k++) free(argv[k]);
            free(argv);
            if (is_new_ref) Py_DECREF(py_arg_list);
            return NULL;
        }
        argv[i+1] = strdup(str_utf8);
        if (!argv[i+1]) {
             for (int k = 0; k <= i; k++) free(argv[k]);
             free(argv);
             if (is_new_ref) Py_DECREF(py_arg_list);
             return PyErr_NoMemory();
        }
    }
    
    // Release the list object if we created it
    if (is_new_ref) {
        Py_DECREF(py_arg_list);
        is_new_ref = 0; 
    }

    // Save standard file descriptors
    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    if (saved_stdout == -1 || saved_stderr == -1) {
        for (int i = 0; i < argc; i++) free(argv[i]);
        free(argv);
        if (saved_stdout != -1) close(saved_stdout);
        if (saved_stderr != -1) close(saved_stderr);
        PyErr_SetString(PyExc_OSError, "Failed to dup stdout/stderr");
        return NULL;
    }

    // Create temporary files for capture
    // Use in-memory buffers if available
    FILE* f_stdout = open_temp_buffer();
    FILE* f_stderr = open_temp_buffer();
    if (!f_stdout || !f_stderr) {
         for (int i = 0; i < argc; i++) free(argv[i]);
         free(argv);
         close(saved_stdout);
         close(saved_stderr);
         if (f_stdout) fclose(f_stdout);
         if (f_stderr) fclose(f_stderr);
         PyErr_SetString(PyExc_OSError, "Failed to create temp files");
         return NULL;
    }

    // Flush Python's buffers just in case
    Py_BEGIN_ALLOW_THREADS
    fflush(stdout);
    fflush(stderr);

    // Redirect stdout/stderr to temp files
    dup2(fileno(f_stdout), STDOUT_FILENO);
    dup2(fileno(f_stderr), STDERR_FILENO);

    exit_code_val = 0;
    
    // Call mm2_main wrapped in setjmp to catch exit() calls
    if (setjmp(exit_env) == 0) {
        // We shouldn't need to reset optind if we were a clean process, 
        // but since we are a library, we might need to?
        // minimap2 uses ketopt, which often initializes locally in main(),
        // but let's hope it doesn't rely on global optind=1.
        // Actually, ketopt takes &ko, initialized to KETOPT_INIT.
        // So it should be re-entrant safe regarding argument parsing.
        mm2_main(argc, argv);
    } else {
        // exit() was called
    }

    // Flush streams before restoring
    fflush(stdout);
    fflush(stderr);
    
    // Restore stdout/stderr
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    Py_END_ALLOW_THREADS

    close(saved_stdout);
    close(saved_stderr);

    // Read captured content
    // Determine size and read
    long sz_out, sz_err;
    
    fseek(f_stdout, 0, SEEK_END);
    sz_out = ftell(f_stdout);
    if (sz_out < 0) sz_out = 0;  // Handle ftell error
    fseek(f_stdout, 0, SEEK_SET);

    fseek(f_stderr, 0, SEEK_END);
    sz_err = ftell(f_stderr);
    if (sz_err < 0) sz_err = 0;  // Handle ftell error
    fseek(f_stderr, 0, SEEK_SET);

    char* buf_out = malloc(sz_out + 1);
    char* buf_err = malloc(sz_err + 1);
    
    if (buf_out) {
        size_t n = 0;
        if (sz_out > 0) n = fread(buf_out, 1, sz_out, f_stdout);
        buf_out[n] = '\0';
    } else {
        buf_out = calloc(1, 1); // Empty string fallback
        if (!buf_out) {
            fclose(f_stdout);
            fclose(f_stderr);
            if (buf_err) free(buf_err);
            return PyErr_NoMemory();
        }
    }

    if (buf_err) {
        size_t n = 0;
        if (sz_err > 0) n = fread(buf_err, 1, sz_err, f_stderr);
        buf_err[n] = '\0';
    } else {
        buf_err = calloc(1, 1);
        if (!buf_err) {
            fclose(f_stdout);
            fclose(f_stderr);
            free(buf_out);
            return PyErr_NoMemory();
        }
    }

    fclose(f_stdout);
    fclose(f_stderr);

    // Free argv
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);

    // Build return tuple (stdout_str, stderr_str)
    // We omit exit_code to match the user request 'out, err = ...'
    // If exit_code indicates failure but stderr is captured, user can check stderr.
    
    PyObject* py_out = PyUnicode_FromString(buf_out);
    PyObject* py_err = PyUnicode_FromString(buf_err);
    
    free(buf_out);
    free(buf_err);

    if (!py_out || !py_err) {
        Py_XDECREF(py_out);
        Py_XDECREF(py_err);
        return NULL;
    }

    PyObject* result = PyTuple_Pack(2, py_out, py_err);
    Py_DECREF(py_out);
    Py_DECREF(py_err);

    return result;
}

static PyMethodDef methods[] = {
    {"main", method_main, METH_VARARGS, "Run minimap2 main with list of arguments. Returns (stdout, stderr)."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "_core", /* module name */
    NULL,
    -1,
    methods
};

PyMODINIT_FUNC PyInit__core(void) {
    return PyModule_Create(&module);
}
