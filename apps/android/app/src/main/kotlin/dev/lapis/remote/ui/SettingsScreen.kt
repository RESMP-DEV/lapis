package dev.lapis.remote.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.lapis.remote.platform.describe

/**
 * Host settings: the port of SettingsView. "Done" saves the trimmed host and
 * refreshes the workspace against it; "Check connection" answers without
 * changing anything. The gateway runs its transport on the IO dispatcher, so
 * the check never blocks composition.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    currentHost: String,
    onDone: (String) -> Unit,
    onCancel: () -> Unit,
) {
    var host by rememberSaveable { mutableStateOf("") }
    var checking by rememberSaveable { mutableStateOf(false) }
    var result by rememberSaveable { mutableStateOf<String?>(null) }

    LaunchedEffect(currentHost) { host = currentHost }

    LaunchedEffect(checking) {
        if (!checking) return@LaunchedEffect
        result = try {
            val listing = dev.lapis.remote.gateway.LapisGateway.build(host.trim()).agents()
            val count = listing.categories.sumOf { it.agents.size }
            val categories = listing.categories.size
            "Connected. $count agent${if (count == 1) "" else "s"} in " +
                "$categories categor${if (categories == 1) "y" else "ies"}."
        } catch (failure: Exception) {
            describe(failure)
        }
        checking = false
    }

    Scaffold(
        containerColor = LapisColors.background,
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        "Settings",
                        style = TextStyle(fontSize = 17.sp, fontWeight = FontWeight.SemiBold),
                    )
                },
                navigationIcon = {
                    TextButton(onClick = onCancel) { Text("Cancel") }
                },
                actions = {
                    TextButton(onClick = { onDone(host.trim()) }) { Text("Done") }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = LapisColors.background,
                    titleContentColor = Color.White,
                ),
            )
        },
    ) { padding ->
        Column(
            Modifier
                .padding(padding)
                .fillMaxSize()
                .padding(horizontal = 20.dp),
        ) {
            Spacer(Modifier.height(12.dp))
            Text(
                "MAC",
                color = LapisColors.quiet,
                style = TextStyle(
                    fontSize = 12.sp,
                    fontWeight = FontWeight.SemiBold,
                    fontFamily = FontFamily.Monospace,
                    letterSpacing = 1.6.sp,
                ),
            )
            Spacer(Modifier.height(6.dp))
            OutlinedTextField(
                value = host,
                onValueChange = { host = it },
                modifier = Modifier.fillMaxWidth(),
                placeholder = { Text("your-mac.your-tailnet.ts.net") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(
                    keyboardType = KeyboardType.Uri,
                    autoCorrectEnabled = false,
                ),
                textStyle = TextStyle(
                    color = Color.White,
                    fontSize = 16.sp,
                    fontFamily = FontFamily.Monospace,
                ),
            )
            Spacer(Modifier.height(4.dp))
            Text(
                "lapis reaches the gateway on your Mac over Tailscale. This device must be " +
                    "signed in to Tailscale with the same account as the Mac; nothing else is " +
                    "needed to sign in.",
                color = LapisColors.quiet,
                style = TextStyle(fontSize = 12.5.sp),
            )
            Spacer(Modifier.height(16.dp))
            HorizontalDivider(color = LapisColors.edge)
            Spacer(Modifier.height(16.dp))
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Button(
                    onClick = {
                        result = null
                        checking = true
                    },
                    enabled = !checking && host.isNotBlank(),
                ) {
                    Text(if (checking) "Checking…" else "Check connection")
                }
                if (checking) {
                    CircularProgressIndicator(
                        Modifier.size(18.dp),
                        strokeWidth = 2.dp,
                        color = LapisColors.quiet,
                    )
                }
            }
            result?.let {
                Spacer(Modifier.height(8.dp))
                Text(it, color = LapisColors.quiet, style = TextStyle(fontSize = 12.5.sp))
            }
        }
    }
}
