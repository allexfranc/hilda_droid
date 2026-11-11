package com.example.injectedclass;

import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.util.Log;
import android.util.Pair;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import javax.crypto.Cipher;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

public class Decrypter {

    private static SecretKey getKeys() {
        try {
            KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
            ks.load(null);
            if (ks.containsAlias("vaultlab_message_key")) {
                return (SecretKey) ks.getKey("vaultlab_message_key", null);
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
        return null;
    }

    private static String decrypt(byte[] blob) throws Exception {
        SecretKey key = getKeys();
        if (key == null) return "";

        byte[] iv = Arrays.copyOfRange(blob, 0, 12);
        byte[] ct = Arrays.copyOfRange(blob, 12, blob.length);
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, iv));
        byte[] pt = cipher.doFinal(ct);
        
        return new String(pt, StandardCharsets.UTF_8);
    }

    private static void dumpMessages(Context context, List<Pair<Integer, String>> messages) {
        File tmpFile = new File(context.getFilesDir(), "msgs_dump.txt.tmp");
        File finalFile = new File(context.getFilesDir(), "msgs_dump.txt");

        try (PrintWriter out = new PrintWriter(new FileWriter(tmpFile))) {
            out.println("dump_count=" + messages.size());

            for (int i = 0; i < messages.size(); i++) {
                Pair<Integer, String> msg = messages.get(i);
                out.println("msg." + i + ".id=" + msg.first);
                out.println("msg." + i + ".body=" + msg.second);
            }

            out.flush();
            out.close();

            if (tmpFile.renameTo(finalFile)) {
                Log.i("VaultLab", "[+] dump written to " + finalFile.getAbsolutePath());
            } else {
                Log.e("VaultLab", "[-] rename failed");
            }

        } catch (Exception e) {
            Log.e("VaultLab", "[-] Exception during dump: " + e.getMessage());
            e.printStackTrace();
        }
    }

    public static void decryptMessagesFromDb(Context context) {
        File dbPath = context.getDatabasePath("messages.db");
        if (!dbPath.exists()) {
            Log.e("VaultLab", "Db not found: " + dbPath.getAbsolutePath());
            return;
        }

        SQLiteDatabase db = SQLiteDatabase.openDatabase(dbPath.getAbsolutePath(), null, SQLiteDatabase.OPEN_READONLY);
        Cursor cursor = db.rawQuery("SELECT id, body FROM messages", null);

        List<Pair<Integer, String>> decryptedList = new ArrayList<>();

        while (cursor.moveToNext()) {
            int id = cursor.getInt(cursor.getColumnIndexOrThrow("id"));
            byte[] blob = cursor.getBlob(cursor.getColumnIndexOrThrow("body"));

            if (blob != null) {
                try {
                    String decryptedText = decrypt(blob);
                    Log.d("VaultLab", "ID: " + id + " | Status: Encrypted | Text: " + decryptedText);
                    decryptedList.add(new Pair<>(id, decryptedText));
                } catch (Exception e) {
                    Log.e("VaultLab", "ID: " + id + " | Error decrypting (Invalid Key?)", e);
                }
            }
        }

        cursor.close();
        db.close();

        dumpMessages(context, decryptedList);
    }
}