/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * Python API for writing a set of data-blocks into a file.
 * Useful for writing out asset-libraries, defines: `bpy.data.libraries.write(...)`.
 */

#include <Python.h>
#include <cstddef>

#include "MEM_guardedalloc.h"

#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_blendfile.h"
#include "BKE_global.h"
#include "BKE_main.hh"
#include "BKE_report.h"

#include "BLO_writefile.hh"

#include "RNA_types.hh"

#include "bpy_capi_utils.h"
#include "bpy_library.h"
#include "bpy_rna.h"

#include "../generic/py_capi_utils.h"
#include "../generic/python_compat.h"

PyDoc_STRVAR(
    bpy_lib_write_doc,
    ".. method:: write(filepath, datablocks, path_remap=False, fake_user=False, compress=False)\n"
    "\n"
    "   Write data-blocks into a blend file.\n"
    "\n"
    "   .. note::\n"
    "\n"
    "      Indirectly referenced data-blocks will be expanded and written too.\n"
    "\n"
    "   :arg filepath: The path to write the blend-file.\n"
    "   :type filepath: string or bytes\n"
    "   :arg datablocks: set of data-blocks (:class:`bpy.types.ID` instances).\n"
    "   :type datablocks: set\n"
    "   :arg path_remap: Optionally remap paths when writing the file:\n"
    "\n"
    "      - ``NONE`` No path manipulation (default).\n"
    "      - ``RELATIVE`` Remap paths that are already relative to the new location.\n"
    "      - ``RELATIVE_ALL`` Remap all paths to be relative to the new location.\n"
    "      - ``ABSOLUTE`` Make all paths absolute on writing.\n"
    "\n"
    "   :type path_remap: string\n"
    "   :arg fake_user: When True, data-blocks will be written with fake-user flag enabled.\n"
    "   :type fake_user: bool\n"
    "   :arg compress: When True, write a compressed blend file.\n"
    "   :type compress: bool\n");
static PyObject *bpy_lib_write(BPy_PropertyRNA *self, PyObject *args, PyObject *kw)
{
  /* args */
  PyC_UnicodeAsBytesAndSize_Data filepath_data = {nullptr};
  char filepath_abs[FILE_MAX];
  PyObject *datablocks = nullptr;

  const PyC_StringEnumItems path_remap_items[] = {
      {BLO_WRITE_PATH_REMAP_NONE, "NONE"},
      {BLO_WRITE_PATH_REMAP_RELATIVE, "RELATIVE"},
      {BLO_WRITE_PATH_REMAP_RELATIVE_ALL, "RELATIVE_ALL"},
      {BLO_WRITE_PATH_REMAP_ABSOLUTE, "ABSOLUTE"},
      {0, nullptr},
  };
  PyC_StringEnum path_remap = {path_remap_items, BLO_WRITE_PATH_REMAP_NONE};

  bool use_fake_user = false, use_compress = false;

  static const char *_keywords[] = {
      "filepath",
      "datablocks",
      "path_remap",
      "fake_user",
      "compress",
      nullptr,
  };
  static _PyArg_Parser _parser = {
      PY_ARG_PARSER_HEAD_COMPAT()
      "O&" /* `filepath` */
      "O!" /* `datablocks` */
      "|$" /* Optional keyword only arguments. */
      "O&" /* `path_remap` */
      "O&" /* `fake_user` */
      "O&" /* `compress` */
      ":write",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kw,
                                        &_parser,
                                        PyC_ParseUnicodeAsBytesAndSize,
                                        &filepath_data,
                                        &PySet_Type,
                                        &datablocks,
                                        PyC_ParseStringEnum,
                                        &path_remap,
                                        PyC_ParseBool,
                                        &use_fake_user,
                                        PyC_ParseBool,
                                        &use_compress))
  {
    return nullptr;
  }

  Main *bmain_src = static_cast<Main *>(self->ptr.data); /* Typically #G_MAIN */
  int write_flags = 0;

  if (use_compress) {
    write_flags |= G_FILE_COMPRESS;
  }

  STRNCPY(filepath_abs, filepath_data.value);
  Py_XDECREF(filepath_data.value_coerce);

  BLI_path_abs(filepath_abs, BKE_main_blendfile_path_from_global());

  BKE_blendfile_write_partial_begin(bmain_src);

  /* array of ID's and backup any data we modify */
  struct IDStore {
    ID *id;
    /* original values */
    short id_flag;
    short id_us;
  } * id_store_array, *id_store;
  int id_store_len = 0;

  PyObject *ret;
  int retval = 0;

  /* collect all id data from the set and store in 'id_store_array' */
  {
    Py_ssize_t pos, hash;
    PyObject *key;

    id_store_array = static_cast<IDStore *>(
        MEM_mallocN(sizeof(*id_store_array) * PySet_Size(datablocks), __func__));
    id_store = id_store_array;

    pos = hash = 0;
    while (_PySet_NextEntry(datablocks, &pos, &key, &hash)) {

      if (!pyrna_id_FromPyObject(key, &id_store->id)) {
        PyErr_Format(PyExc_TypeError, "Expected an ID type, not %.200s", Py_TYPE(key)->tp_name);
        ret = nullptr;
        goto finally;
      }
      else {
        id_store->id_flag = id_store->id->flag;
        id_store->id_us = id_store->id->us;

        if (use_fake_user) {
          id_store->id->flag |= LIB_FAKEUSER;
        }
        id_store->id->us = 1;

        BKE_blendfile_write_partial_tag_ID(id_store->id, true);

        id_store_len += 1;
        id_store++;
      }
    }
  }

  /* write blend */
  ReportList reports;

  BKE_reports_init(&reports, RPT_STORE);
  retval = BKE_blendfile_write_partial(
      bmain_src, filepath_abs, write_flags, path_remap.value_found, &reports);

  /* cleanup state */
  BKE_blendfile_write_partial_end(bmain_src);

  if (retval) {
    BKE_reports_print(&reports, RPT_ERROR_ALL);
    ret = Py_None;
    Py_INCREF(ret);
  }
  else {
    if (BPy_reports_to_error(&reports, PyExc_IOError, false) == 0) {
      PyErr_SetString(PyExc_IOError, "Unknown error writing library data");
    }
    ret = nullptr;
  }

  BKE_reports_free(&reports);

finally:

  /* clear all flags for ID's added to the store (may run on error too) */
  id_store = id_store_array;

  for (int i = 0; i < id_store_len; id_store++, i++) {

    if (use_fake_user) {
      if ((id_store->id_flag & LIB_FAKEUSER) == 0) {
        id_store->id->flag &= ~LIB_FAKEUSER;
      }
    }

    id_store->id->us = id_store->id_us;

    BKE_blendfile_write_partial_tag_ID(id_store->id, false);
  }

  MEM_freeN(id_store_array);

  return ret;
}

#if (defined(__GNUC__) && !defined(__clang__))
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

PyMethodDef BPY_library_write_method_def = {
    "write",
    (PyCFunction)bpy_lib_write,
    METH_VARARGS | METH_KEYWORDS,
    bpy_lib_write_doc,
};

#if (defined(__GNUC__) && !defined(__clang__))
#  pragma GCC diagnostic pop
#endif
