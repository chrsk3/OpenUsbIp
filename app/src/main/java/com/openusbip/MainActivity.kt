package com.openusbip

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {

    external fun startUsbIpServer(
        fd: Int, port: Int, vid: Int, pid: Int,
        cls: Int, sub: Int, proto: Int, intfTriples: IntArray
    ): String

    external fun stopUsbIpServer()

    companion object {
        private const val ACTION_USB_PERMISSION = "com.openusbip.USB_PERMISSION"
        init { System.loadLibrary("openusbip") }
    }

    private lateinit var usbManager: UsbManager
    private lateinit var statusTv: TextView
    private var activeConnection: UsbDeviceConnection? = null
    private var serverThread: Thread? = null

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == ACTION_USB_PERMISSION) {
                val device: UsbDevice? = if (Build.VERSION.SDK_INT >= 33) {
                    intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                }
                val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                if (granted && device != null) {
                    launchServer(device)
                } else {
                    statusTv.text = "Permissão USB negada."
                }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        usbManager = getSystemService(USB_SERVICE) as UsbManager

        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
        }

        statusTv = TextView(this).apply {
            textSize = 15f
            text = "Conecte o Motorola Signature via OTG e toque em Iniciar."
        }

        val btnStart = Button(this).apply {
            text = "Iniciar Servidor USB/IP (porta 3240)"
            setOnClickListener { requestAndStart() }
        }

        val btnStop = Button(this).apply {
            text = "Parar Servidor"
            setOnClickListener { stopServer() }
        }

        layout.addView(btnStart)
        layout.addView(btnStop)
        layout.addView(statusTv)
        setContentView(layout)

        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(usbReceiver, filter, RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(usbReceiver, filter)
        }
    }

    private fun requestAndStart() {
        val device = usbManager.deviceList.values.firstOrNull()
        if (device == null) {
            statusTv.text = "Nenhum dispositivo USB detectado no OTG."
            return
        }
        if (usbManager.hasPermission(device)) {
            launchServer(device)
        } else {
            val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
            val pi = PendingIntent.getBroadcast(this, 0, Intent(ACTION_USB_PERMISSION), flags)
            usbManager.requestPermission(device, pi)
            statusTv.text = "Aguardando autorização USB..."
        }
    }

    private fun launchServer(device: UsbDevice) {
        stopServer()
        val conn = usbManager.openDevice(device)
        if (conn == null) {
            statusTv.text = "Falha ao abrir dispositivo USB."
            return
        }
        activeConnection = conn

        val triples = IntArray(device.interfaceCount * 3)
        for (i in 0 until device.interfaceCount) {
            val intf = device.getInterface(i)
            conn.claimInterface(intf, true)
            triples[i * 3] = intf.interfaceClass
            triples[i * 3 + 1] = intf.interfaceSubclass
            triples[i * 3 + 2] = intf.interfaceProtocol
        }

        val fd = conn.fileDescriptor
        val vidHex = device.vendorId.toString(16).padStart(4, '0')
        val pidHex = device.productId.toString(16).padStart(4, '0')
        statusTv.text = "Rodando em 127.0.0.1:3240\nDispositivo: ${device.productName ?: "USB"}\nVID:$vidHex PID:$pidHex | FD=$fd | Interfaces=${device.interfaceCount}"

        serverThread = Thread {
            val msg = startUsbIpServer(
                fd, 3240, device.vendorId, device.productId,
                device.deviceClass, device.deviceSubclass, device.deviceProtocol, triples
            )
            runOnUiThread { statusTv.text = "Status: $msg" }
        }.apply { start() }
    }

    private fun stopServer() {
        stopUsbIpServer()
        activeConnection?.close()
        activeConnection = null
    }

    override fun onDestroy() {
        super.onDestroy()
        stopServer()
        unregisterReceiver(usbReceiver)
    }
}
