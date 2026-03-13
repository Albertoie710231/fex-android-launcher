package com.mediatek.steamlauncher

import android.app.Activity
import android.os.Handler
import android.widget.EditText
import `in`.dragonbra.javasteam.steam.authentication.IAuthenticator
import java.util.concurrent.CompletableFuture

/**
 * IAuthenticator implementation that shows Android AlertDialogs
 * for Steam Guard / 2FA code entry.
 */
class DialogAuthenticator(
    private val activity: Activity,
    private val handler: Handler
) : IAuthenticator {

    override fun getDeviceCode(previousCodeWasIncorrect: Boolean): CompletableFuture<String> {
        val title = if (previousCodeWasIncorrect)
            "Steam Guard (incorrect, retry)"
        else
            "Steam Guard — Mobile Authenticator"
        return showCodeDialog(title, "Enter the code from your Steam Mobile Authenticator")
    }

    override fun getEmailCode(email: String?, previousCodeWasIncorrect: Boolean): CompletableFuture<String> {
        val title = if (previousCodeWasIncorrect)
            "Steam Guard Email (incorrect, retry)"
        else
            "Steam Guard — Email Code"
        return showCodeDialog(title, "Enter the code sent to ${email ?: "your email"}")
    }

    override fun acceptDeviceConfirmation(): CompletableFuture<Boolean> {
        val future = CompletableFuture<Boolean>()
        handler.post {
            android.app.AlertDialog.Builder(activity)
                .setTitle("Steam Guard")
                .setMessage("Please confirm this login on your Steam Mobile App, then tap OK.")
                .setPositiveButton("OK") { _, _ -> future.complete(true) }
                .setNegativeButton("Cancel") { _, _ -> future.complete(false) }
                .setCancelable(false)
                .show()
        }
        return future
    }

    private fun showCodeDialog(title: String, message: String): CompletableFuture<String> {
        val future = CompletableFuture<String>()
        handler.post {
            val input = EditText(activity).apply {
                hint = "Code"
                setPadding(50, 20, 50, 20)
            }
            android.app.AlertDialog.Builder(activity)
                .setTitle(title)
                .setMessage(message)
                .setView(input)
                .setPositiveButton("Submit") { _, _ ->
                    future.complete(input.text.toString().trim())
                }
                .setNegativeButton("Cancel") { _, _ ->
                    future.complete("")
                }
                .setCancelable(false)
                .show()
        }
        return future
    }
}
