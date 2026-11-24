package com.openusbip

import android.hardware.usb.UsbManager
import android.os.Bundle
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {
    external fun startUsbIpServer(fd: Int, port: Int): String

    companion object {
        init {
            System.loadLibrary("openusbip")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val usbManager = getSystemService(USB_SERVICE) as UsbManager
        val devices = usbManager.deviceList.values.joinToString("\n") {
            "${it.productName ?: "USB"} (VID:${it.vendorId.toString(16)} PID:${it.productId.toString(16)})"
        }.ifEmpty { "Nenhum dispositivo USB conectado" }

        val tv = TextView(this).apply {
            setPadding(40, 40, 40, 40)
            text = "OpenUsbIp\n\nStatus NDK: ${startUsbIpServer(-1, 3240)}\n\nDispositivos:\n$devices"
        }
        setContentView(tv)
    }
}
