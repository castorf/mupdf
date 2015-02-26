#include "mupdf/pdf.h"

static void
pdf_clean_stream_object(fz_context *ctx, pdf_document *doc, pdf_obj *obj, pdf_obj *orig_res, fz_cookie *cookie, int own_res)
{
	pdf_processor *proc_buffer = NULL;
	pdf_processor *proc_filter = NULL;
	pdf_obj *res = NULL;
	pdf_obj *ref = NULL;
	fz_buffer *buffer;

	if (!obj)
		return;

	fz_var(res);
	fz_var(ref);
	fz_var(proc_buffer);
	fz_var(proc_filter);

	buffer = fz_new_buffer(ctx, 1024);

	fz_try(ctx)
	{
		if (own_res)
		{
			pdf_obj *r = pdf_dict_gets(ctx, obj, "Resources");
			if (r)
				orig_res = r;
		}

		res = pdf_new_dict(ctx, doc, 1);

		proc_buffer = pdf_new_buffer_processor(ctx, buffer);
		proc_filter = pdf_new_filter_processor(ctx, proc_buffer, doc, orig_res, res);

		pdf_process_contents(ctx, proc_filter, doc, orig_res, obj, cookie);

		pdf_update_stream(ctx, doc, obj, buffer, 0);

		if (own_res)
		{
			ref = pdf_new_ref(ctx, doc, res);
			pdf_dict_puts(ctx, obj, "Resources", ref);
		}
	}
	fz_always(ctx)
	{
		pdf_drop_processor(ctx, proc_filter);
		pdf_drop_processor(ctx, proc_buffer);
		fz_drop_buffer(ctx, buffer);
		pdf_drop_obj(ctx, res);
		pdf_drop_obj(ctx, ref);
	}
	fz_catch(ctx)
	{
		fz_rethrow_message(ctx, "Failed while cleaning xobject");
	}
}

static void
pdf_clean_type3(fz_context *ctx, pdf_document *doc, pdf_obj *obj, pdf_obj *orig_res, fz_cookie *cookie)
{
	pdf_processor *proc_buffer = NULL;
	pdf_processor *proc_filter = NULL;
	pdf_obj *res = NULL;
	pdf_obj *ref = NULL;
	pdf_obj *charprocs;
	int i, l;

	fz_var(res);
	fz_var(ref);
	fz_var(proc_buffer);
	fz_var(proc_filter);

	fz_try(ctx)
	{
		res = pdf_dict_gets(ctx, obj, "Resources");
		if (res)
			orig_res = res;
		res = NULL;

		res = pdf_new_dict(ctx, doc, 1);

		charprocs = pdf_dict_gets(ctx, obj, "CharProcs");
		l = pdf_dict_len(ctx, charprocs);

		for (i = 0; i < l; i++)
		{
			pdf_obj *val = pdf_dict_get_val(ctx, charprocs, i);
			fz_buffer *buffer = fz_new_buffer(ctx, 1024);
			fz_try(ctx)
			{
				proc_buffer = pdf_new_buffer_processor(ctx, buffer);
				proc_filter = pdf_new_filter_processor(ctx, proc_buffer, doc, orig_res, res);

				pdf_process_contents(ctx, proc_filter, doc, orig_res, val, cookie);

				pdf_update_stream(ctx, doc, val, buffer, 0);
			}
			fz_always(ctx)
			{
				pdf_drop_processor(ctx, proc_filter);
				pdf_drop_processor(ctx, proc_buffer);
				fz_drop_buffer(ctx, buffer);
			}
			fz_catch(ctx)
			{
				fz_rethrow(ctx);
			}
		}

		/* ProcSet - no cleaning possible. Inherit this from the old dict. */
		pdf_dict_puts(ctx, res, "ProcSet", pdf_dict_gets(ctx, orig_res, "ProcSet"));

		ref = pdf_new_ref(ctx, doc, res);
		pdf_dict_puts(ctx, obj, "Resources", ref);
	}
	fz_always(ctx)
	{
		pdf_drop_obj(ctx, res);
		pdf_drop_obj(ctx, ref);
	}
	fz_catch(ctx)
	{
		fz_rethrow_message(ctx, "Failed while cleaning xobject");
	}
}

void pdf_clean_page_contents(fz_context *ctx, pdf_document *doc, pdf_page *page, fz_cookie *cookie, pdf_page_contents_process_fn *proc_fn, void *proc_arg)
{
	pdf_processor *proc_buffer = NULL;
	pdf_processor *proc_filter = NULL;
	pdf_obj *new_obj = NULL;
	pdf_obj *new_ref = NULL;
	pdf_obj *res = NULL;
	pdf_obj *ref = NULL;
	pdf_obj *obj;
	pdf_obj *contents;
	fz_buffer *buffer;

	fz_var(new_obj);
	fz_var(new_ref);
	fz_var(res);
	fz_var(ref);
	fz_var(proc_buffer);
	fz_var(proc_filter);

	buffer = fz_new_buffer(ctx, 1024);

	fz_try(ctx)
	{
		res = pdf_new_dict(ctx, doc, 1);

		proc_buffer = pdf_new_buffer_processor(ctx, buffer);
		proc_filter = pdf_new_filter_processor(ctx, proc_buffer, doc, page->resources, res);

		pdf_process_contents(ctx, proc_filter, doc, page->resources, page->contents, cookie);

		contents = page->contents;
		if (pdf_is_array(ctx, contents))
		{
			/* create a new object to replace the array */
			new_obj = pdf_new_dict(ctx, doc, 1);
			new_ref = pdf_new_ref(ctx, doc, new_obj);
			page->contents = contents = new_ref;
		}
		else
		{
			pdf_dict_dels(ctx, contents, "Filter");
			pdf_dict_dels(ctx, contents, "DecodeParms");
		}

		/* Now deal with resources. The spec allows for Type3 fonts and form
		 * XObjects to omit a resource dictionary and look in the parent.
		 * Avoid that by flattening here as part of the cleaning. This could
		 * conceivably cause changes in rendering, but we don't care. */

		/* ExtGState */
		obj = pdf_dict_gets(ctx, res, "ExtGState");
		if (obj)
		{
			int i, l;

			l = pdf_dict_len(ctx, obj);
			for (i = 0; i < l; i++)
			{
				pdf_obj *o = pdf_dict_gets(ctx, pdf_dict_get_val(ctx, obj, i), "SMask");

				if (!o)
					continue;
				o = pdf_dict_gets(ctx, o, "G");
				if (!o)
					continue;

				/* Transparency group XObject */
				pdf_clean_stream_object(ctx, doc, o, page->resources, cookie, 1);
			}
		}

		/* ColorSpace - no cleaning possible */

		/* Pattern */
		obj = pdf_dict_gets(ctx, res, "Pattern");
		if (obj)
		{
			int i, l;

			l = pdf_dict_len(ctx, obj);
			for (i = 0; i < l; i++)
			{
				pdf_obj *pat = pdf_dict_get_val(ctx, obj, i);

				if (!pat)
					continue;
				if (pdf_to_int(ctx, pdf_dict_gets(ctx, pat, "PatternType")) == 1)
					pdf_clean_stream_object(ctx, doc, pat, page->resources, cookie, 0);
			}
		}

		/* Shading - no cleaning possible */

		/* XObject */
		obj = pdf_dict_gets(ctx, res, "XObject");
		if (obj)
		{
			int i, l;

			l = pdf_dict_len(ctx, obj);
			for (i = 0; i < l; i++)
			{
				pdf_obj *xobj = pdf_dict_get_val(ctx, obj, i);

				if (strcmp(pdf_to_name(ctx, pdf_dict_gets(ctx, xobj, "Subtype")), "Form"))
					continue;

				pdf_clean_stream_object(ctx, doc, xobj, page->resources, cookie, 1);
			}
		}

		/* Font */
		obj = pdf_dict_gets(ctx, res, "Font");
		if (obj)
		{
			int i, l;

			l = pdf_dict_len(ctx, obj);
			for (i = 0; i < l; i++)
			{
				pdf_obj *o = pdf_dict_get_val(ctx, obj, i);

				if (!strcmp(pdf_to_name(ctx, pdf_dict_gets(ctx, o, "Subtype")), "Type3"))
				{
					pdf_clean_type3(ctx, doc, o, page->resources, cookie);
				}
			}
		}

		/* ProcSet - no cleaning possible. Inherit this from the old dict. */
		obj = pdf_dict_gets(ctx, page->resources, "ProcSet");
		if (obj)
			pdf_dict_puts(ctx, res, "ProcSet", obj);

		/* Properties - no cleaning possible. */

		if (proc_fn)
			(*proc_fn)(ctx, buffer, res, proc_arg);

		pdf_update_stream(ctx, doc, contents, buffer, 0);
		pdf_drop_obj(ctx, page->resources);
		ref = pdf_new_ref(ctx, doc, res);
		page->resources = pdf_keep_obj(ctx, ref);
		pdf_dict_puts(ctx, page->me, "Resources", ref);
	}
	fz_always(ctx)
	{
		pdf_drop_processor(ctx, proc_filter);
		pdf_drop_processor(ctx, proc_buffer);
		fz_drop_buffer(ctx, buffer);
		pdf_drop_obj(ctx, new_obj);
		pdf_drop_obj(ctx, new_ref);
		pdf_drop_obj(ctx, res);
		pdf_drop_obj(ctx, ref);
	}
	fz_catch(ctx)
	{
		fz_rethrow_message(ctx, "Failed while cleaning page");
	}
}
