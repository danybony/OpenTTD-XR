/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

package org.openttd.xr;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/** Lets the app a tester shares with read the test logs and crash reports, and nothing else. */
public class TestLogProvider extends ContentProvider {
	static final String AUTHORITY = "org.openttd.xr.testlog";
	static final String CRASH_LOG = "crash\\d+\\.json\\.log";
	private static final String CRASH_SAVE = "crash\\d+\\.sav";

	/** Where the game keeps its saves and crash reports. */
	static File personalDir(Context context) {
		return new File(context.getFilesDir(), "data/openttd");
	}

	static Uri uriFor(File file) {
		return new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath(file.getName()).build();
	}

	static String typeOf(String name) {
		return name.endsWith(".log") ? "text/plain" : "application/octet-stream";
	}

	/** The file a name stands for, if it is one that may be shared. */
	private File fileFor(Uri uri) throws FileNotFoundException {
		final String name = uri.getLastPathSegment();
		if (name != null && (name.equals(OpenTTDActivity.TEST_LOG) || name.equals(OpenTTDActivity.PREVIOUS_TEST_LOG))) {
			return new File(getContext().getFilesDir(), name);
		}
		if (name != null && (name.matches(CRASH_LOG) || name.matches(CRASH_SAVE))) return new File(personalDir(getContext()), name);
		throw new FileNotFoundException(String.valueOf(uri));
	}

	@Override
	public boolean onCreate() {
		return true;
	}

	@Override
	public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
		return ParcelFileDescriptor.open(fileFor(uri), ParcelFileDescriptor.MODE_READ_ONLY);
	}

	@Override
	public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
		final File file;
		try {
			file = fileFor(uri);
		} catch (FileNotFoundException e) {
			return null;
		}
		final String[] columns = projection != null ? projection : new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
		final MatrixCursor cursor = new MatrixCursor(columns, 1);
		final Object[] row = new Object[columns.length];
		for (int i = 0; i < columns.length; i++) {
			if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) row[i] = file.getName();
			if (OpenableColumns.SIZE.equals(columns[i])) row[i] = file.length();
		}
		cursor.addRow(row);
		return cursor;
	}

	@Override
	public String getType(Uri uri) {
		final String name = uri.getLastPathSegment();
		return name == null ? null : typeOf(name);
	}

	@Override
	public Uri insert(Uri uri, ContentValues values) {
		throw new UnsupportedOperationException();
	}

	@Override
	public int delete(Uri uri, String selection, String[] selectionArgs) {
		throw new UnsupportedOperationException();
	}

	@Override
	public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
		throw new UnsupportedOperationException();
	}
}
