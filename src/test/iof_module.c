/* Copyright (C) 2017 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted for any purpose (including commercial purposes)
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the
 *    documentation and/or materials provided with the distribution.
 *
 * 3. In addition, redistributions of modified forms of the source or binary
 *    code must carry prominent notices stating that the original code was
 *    changed and the date of the change.
 *
 *  4. All publications or advertising materials mentioning features or use of
 *     this software are asked, but not required, to acknowledge that it was
 *     developed by Intel Corporation and credit the contributors.
 *
 * 5. Neither the name of Intel Corporation, nor the name of any Contributor
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*** Python/C Shim used for testing ***/

#include <sys/ioctl.h>
#include <fcntl.h>
#include <python3.4m/Python.h>

#define BUF_SIZE 4096

/* No support for Python versions < 3 */
#define PyInt_FromLong(x) (PyLong_FromLong((x)))
#define PyInt_AsLong(x) (PyLong_AsLong((x)))
#define PyString_AsString(x) (PyBytes_AsString(x))
#define PyString_Size(x) (PyBytes_Size(x))
#define Py_InitModule4 Py_InitModule4_64

struct module_state {
	PyObject *error;
};

#define GETSTATE(m) ((struct module_state *)PyModule_GetState(m))

/* Test low-level POSIX APIs */
static PyObject *open_test_file(PyObject *self, PyObject *args)
{
	char *mount_dir;
	char test_file[BUF_SIZE];
	int fd;
	size_t len;

	if (!PyArg_ParseTuple(args, "s", &mount_dir))
		Py_RETURN_NONE;

	len = strlen(mount_dir);
	snprintf(test_file, BUF_SIZE - len, "%s/posix_test_file", mount_dir);
	test_file[BUF_SIZE - 1] = 0;

	errno = 0;
	fd = open(test_file, O_RDWR | O_CREAT, 0600);

	if (errno == 0) {
		printf("\nOpened %s, fd = %d\n", test_file, fd);
		return PyInt_FromLong(fd);
	}

	printf("Open file errno = %s\n", strerror(errno));
	Py_RETURN_NONE;
}

static PyObject *test_write_file(PyObject *self, PyObject *args)
{
	int fd;
	char write_buf[BUF_SIZE];
	ssize_t bytes;
	size_t len;

	if (!PyArg_ParseTuple(args, "i", &fd))
		Py_RETURN_NONE;

	snprintf(write_buf, BUF_SIZE, "Writing to a test file\n");
	len = strlen(write_buf);

	printf("Writing: '%s' to fd = %d\n", write_buf, fd);
	errno = 0;
	bytes = write(fd, write_buf, len);

	if (bytes != len) {
		printf("Wrote %zd bytes, expected %zu\n", bytes, len);
		Py_RETURN_NONE;
	}

	if (errno == 0) {
		printf("Wrote %zd bytes, expected %zu %d %s\n", bytes, len,
		       errno, strerror(errno));
		return PyInt_FromLong(fd);
	}

	printf("Write file errno = %s\n", strerror(errno));
	Py_RETURN_NONE;
}

static PyObject *test_read_file(PyObject *self, PyObject *args)
{
	int fd;
	char read_buf[BUF_SIZE] = {0};
	ssize_t bytes;

	if (!PyArg_ParseTuple(args, "i", &fd))
		Py_RETURN_NONE;

	if (!test_write_file(self, args))
		Py_RETURN_NONE;

	if (lseek(fd, 0, SEEK_SET) < 0) {
		printf("lseek error %s after read\n", strerror(errno));
		Py_RETURN_NONE;
	}

	printf("Reading from fd = %d\n", fd);
	errno = 0;
	bytes = read(fd, read_buf, BUF_SIZE - 1);

	if (errno == 0) {
		printf("Read %zd bytes\n", bytes);
		printf("Read: '%s'\n", read_buf);
		return PyInt_FromLong(fd);
	}

	printf("Read file errno = %s\n", strerror(errno));
	Py_RETURN_NONE;
}

static PyObject *close_test_file(PyObject *self, PyObject *args)
{
	int fd, rc;

	if (!PyArg_ParseTuple(args, "i", &fd))
		Py_RETURN_NONE;

	errno = 0;
	rc = close(fd);

	if (errno == 0) {
		printf("Closed fd  = %d\n", fd);
		return PyInt_FromLong(rc);
	}

	printf("Close file errno = %s\n", strerror(errno));
	Py_RETURN_NONE;
}

static PyMethodDef iofMethods[] = {
	{ "open_test_file", (PyCFunction)open_test_file, METH_VARARGS, NULL },
	{ "test_write_file", (PyCFunction)test_write_file, METH_VARARGS, NULL },
	{ "test_read_file", (PyCFunction)test_read_file, METH_VARARGS, NULL },
	{ "close_test_file", (PyCFunction)close_test_file, METH_VARARGS, NULL },
};

static int iofmod_traverse(PyObject *m, visitproc visit, void *arg)
{
	Py_VISIT(GETSTATE(m)->error);
	return 0;
}

static int iofmod_clear(PyObject *m)
{
	Py_CLEAR(GETSTATE(m)->error);
	return 0;
}

static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	"iofmod",
	NULL,
	sizeof(struct module_state),
	iofMethods,
	NULL,
	iofmod_traverse,
	iofmod_clear,
	NULL
};

PyMODINIT_FUNC PyInit_iofmod(void)
{
	PyObject *module;
	struct module_state *st;

	module = PyModule_Create(&moduledef);

	st = GETSTATE(module);
	st->error = PyErr_NewException("myextension.Error", NULL, NULL);
	if (!st->error) {
		Py_DECREF(module);
			return NULL;
	}

	return module;
}
