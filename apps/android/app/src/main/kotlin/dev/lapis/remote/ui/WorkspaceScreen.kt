package dev.lapis.remote.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Outline
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.lapis.remote.gateway.Agent
import dev.lapis.remote.gateway.WorkspaceListing

/**
 * The category/agent list: the milestone-A port of AgentListView's content
 * states. Cards keep the desktop's command-room card (cut corners, harness
 * edge, status light). Opening an agent, swipe-to-close and the new-agent
 * sheet arrive with later milestones.
 */

/** A rectangle with its top-left and bottom-right corners cut at 45 degrees (the iOS `Chamfered`). */
class Chamfered(private val cut: Dp) : Shape {
    override fun createOutline(
        size: Size,
        layoutDirection: LayoutDirection,
        density: Density,
    ): Outline {
        val c = with(density) { cut.toPx() }
        return Outline.Generic(
            Path().apply {
                moveTo(c, 0f)
                lineTo(size.width, 0f)
                lineTo(size.width, size.height - c)
                lineTo(size.width - c, size.height)
                lineTo(0f, size.height)
                lineTo(0f, c)
                close()
            },
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun WorkspaceScreen(
    host: String,
    listing: WorkspaceListing?,
    error: String?,
    refreshing: Boolean,
    onRefresh: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    Scaffold(
        containerColor = LapisColors.background,
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        "lapis",
                        style = TextStyle(fontWeight = FontWeight.SemiBold, fontSize = 17.sp),
                    )
                },
                actions = {
                    TextButton(onClick = onOpenSettings) { Text("Settings") }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = LapisColors.background,
                    titleContentColor = Color.White,
                ),
            )
        },
    ) { padding ->
        Box(
            Modifier
                .padding(padding)
                .fillMaxSize(),
        ) {
            when {
                host.isEmpty() -> ConnectToMac(onOpenSettings)
                listing != null -> WorkspaceList(listing, error, onRefresh)
                error != null -> CantReach(error, onRefresh, onOpenSettings)
                else -> Row(
                    modifier = Modifier.align(Alignment.Center),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    if (refreshing) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(18.dp),
                            strokeWidth = 2.dp,
                            color = LapisColors.quiet,
                        )
                    }
                    Text(
                        "Connecting to $host",
                        color = LapisColors.quiet,
                        style = TextStyle(fontSize = 14.sp),
                    )
                }
            }
        }
    }
}

@Composable
private fun ConnectToMac(onOpenSettings: () -> Unit) {
    Column(
        modifier = Modifier.fillMaxSize().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            "Connect to your Mac",
            color = Color.White,
            style = TextStyle(fontSize = 20.sp, fontWeight = FontWeight.SemiBold),
        )
        Spacer(Modifier.height(8.dp))
        Text(
            "Enter the Mac's Tailscale name in Settings.",
            color = LapisColors.quiet,
            style = TextStyle(fontSize = 14.sp),
        )
        Spacer(Modifier.height(16.dp))
        Button(onClick = onOpenSettings) { Text("Settings") }
    }
}

@Composable
private fun CantReach(error: String, onRefresh: () -> Unit, onOpenSettings: () -> Unit) {
    Column(
        modifier = Modifier.fillMaxSize().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            "Can't reach lapis",
            color = Color.White,
            style = TextStyle(fontSize = 20.sp, fontWeight = FontWeight.SemiBold),
        )
        Spacer(Modifier.height(8.dp))
        Text(
            error,
            color = LapisColors.quiet,
            style = TextStyle(fontSize = 14.sp),
        )
        Spacer(Modifier.height(16.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = onRefresh) { Text("Try again") }
            TextButton(onClick = onOpenSettings) { Text("Settings") }
        }
    }
}

@Composable
private fun WorkspaceList(listing: WorkspaceListing, error: String?, onRefresh: () -> Unit) {
    LazyColumn(Modifier.fillMaxSize()) {
        if (error != null) {
            item(key = "error") {
                Text(
                    error,
                    // The iOS error row's orange: advisory, not fatal.
                    color = Color(0.95f, 0.62f, 0.24f),
                    style = TextStyle(fontSize = 12.5.sp),
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 8.dp),
                )
            }
        }
        listing.categories.forEach { category ->
            item(key = "category-${category.id}") {
                CategoryHeader(category.name)
            }
            if (category.agents.isEmpty()) {
                item(key = "empty-${category.id}") {
                    Text(
                        "No agents",
                        color = LapisColors.quiet,
                        style = TextStyle(fontSize = 12.5.sp),
                        modifier = Modifier.padding(start = 20.dp, bottom = 6.dp),
                    )
                }
            }
            items(category.agents, key = { it.id }) { agent ->
                // Cards are the list's navigation affordance; the stage opens
                // in milestone B, so tapping refreshes for now.
                AgentCard(agent = agent, onClick = onRefresh)
            }
        }
    }
}

@Composable
private fun CategoryHeader(name: String) {
    Text(
        name.uppercase(),
        color = LapisColors.quiet,
        style = TextStyle(
            fontSize = 12.sp,
            fontWeight = FontWeight.SemiBold,
            fontFamily = FontFamily.Monospace,
            letterSpacing = 1.6.sp,
        ),
        modifier = Modifier.padding(start = 20.dp, top = 18.dp, bottom = 4.dp),
    )
}

/** A card with cut corners and a harness-tinted edge: identity, place in path form, a status light. */
@Composable
private fun AgentCard(agent: Agent, onClick: () -> Unit) {
    val accent = LapisColors.harness(agent.harness)
    Box(
        modifier = Modifier
            .padding(horizontal = 16.dp, vertical = 5.dp)
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .background(
                Brush.verticalGradient(listOf(LapisColors.panel, LapisColors.panelDeep)),
                Chamfered(12.dp),
            )
            .border(1.dp, accent.copy(alpha = 0.28f), Chamfered(12.dp))
            .padding(horizontal = 14.dp, vertical = 12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            HarnessBadge(agent.harness, accent)
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(
                    agent.title,
                    color = Color.White,
                    style = TextStyle(fontSize = 17.sp, fontWeight = FontWeight.SemiBold),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(2.dp))
                PlaceText(agent)
            }
            Spacer(Modifier.width(8.dp))
            StatusLight(agent)
            Spacer(Modifier.width(6.dp))
            Text(
                "›",
                color = LapisColors.quiet.copy(alpha = 0.7f),
                style = TextStyle(fontSize = 14.sp, fontWeight = FontWeight.Bold),
            )
        }
    }
}

/** "~/dev/infinity" here; the machine in the accent colour elsewhere, as the iOS placeText. */
@Composable
private fun PlaceText(agent: Agent) {
    val accent = LapisColors.harness(agent.harness)
    val machine = agent.machine?.takeIf { it.isNotEmpty() }
    val styled = when {
        machine != null && agent.location.startsWith("$machine:") -> buildAnnotatedString {
            withStyle(SpanStyle(color = accent)) { append("$machine:") }
            withStyle(SpanStyle(color = LapisColors.quiet)) {
                append(agent.location.substring(machine.length + 1))
            }
        }
        else -> buildAnnotatedString {
            withStyle(SpanStyle(color = LapisColors.quiet)) { append(agent.location) }
        }
    }
    Text(
        styled,
        style = TextStyle(fontSize = 12.5.sp, fontFamily = FontFamily.Monospace),
        maxLines = 1,
        overflow = TextOverflow.StartEllipsis,
    )
}

@Composable
private fun HarnessBadge(harness: String, accent: Color) {
    Box(
        modifier = Modifier
            .size(42.dp)
            .background(Color.Black, Chamfered(7.dp))
            .border(1.dp, accent.copy(alpha = 0.35f), Chamfered(7.dp)),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            harness.take(2).uppercase(),
            color = accent,
            style = TextStyle(
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace,
            ),
        )
    }
}

@Composable
private fun StatusLight(agent: Agent) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        if (agent.onPhone) {
            // A small phone silhouette standing in for the iOS SF Symbol.
            Box(
                Modifier
                    .size(width = 8.dp, height = 12.dp)
                    .border(1.dp, LapisColors.accent, RoundedCornerShape(2.dp)),
            )
        }
        if (!agent.running) {
            Text(
                "OFF",
                color = LapisColors.quiet,
                style = TextStyle(
                    fontSize = 10.sp,
                    fontWeight = FontWeight.SemiBold,
                    fontFamily = FontFamily.Monospace,
                    letterSpacing = 1.sp,
                ),
            )
        }
        androidx.compose.foundation.Canvas(Modifier.size(7.dp)) {
            // A diamond light: the iOS rectangle rotated 45 degrees.
            drawPath(
                Path().apply {
                    moveTo(size.width / 2, 0f)
                    lineTo(size.width, size.height / 2)
                    lineTo(size.width / 2, size.height)
                    lineTo(0f, size.height / 2)
                    close()
                },
                color = if (agent.running) {
                    LapisColors.live
                } else {
                    LapisColors.quiet.copy(alpha = 0.35f)
                },
            )
        }
    }
}
