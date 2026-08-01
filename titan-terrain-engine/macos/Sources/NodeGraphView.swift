// Node graph drawer: canvas, node cards, wires, palette, inspector.
//
// The interaction model is deliberately unoriginal. Node editors are a mature
// form and their users move between them, so the conventions that recur across
// Blender, Substance Designer, Houdini, Unreal and Gaea are treated as the
// specification and the middle ground is what they agree on:
//
//   - Data flows left to right, inputs on the left edge, output on the right.
//   - Drag from a port to wire two nodes; drop a wire on empty canvas and a
//     search palette opens with the new node already connected.
//   - Double-click empty canvas (or press A) to search for a node.
//   - Drop an unconnected node onto a wire to splice it into the chain.
//   - Every node carries a thumbnail of its own result, because a terrain
//     graph is unreadable without seeing what each step did (Substance, Gaea).
//   - Any node can be soloed into the viewport without rewiring, the way
//     Gaea's pin and Substance's 2D view work.
//   - Space-drag or scroll to pan, pinch or ⌘± to zoom, ⌘0 to fit.
//
// The one thing not borrowed is the graph/stack split. Gaea answers the
// "graphs get unreadable" problem with a per-node modifier stack; here the
// layer stack already exists as the simple path, so the two are views of the
// same document instead — a linear graph converts back to a stack, and the
// drawer opens on the stack laid out as the line it already is.

import AppKit
import SwiftUI

// ---------------------------------------------------------------------------
// Geometry — computed, not measured, so wires and hit-testing agree exactly
// ---------------------------------------------------------------------------

enum NodeGeometry {
    static let width: CGFloat = 178
    static let headerHeight: CGFloat = 26
    // Deliberately small. A thumbnail is a glance, not a preview — the
    // viewport is the preview. At 84 the image was most of the card, which
    // pushed the ports to the extremes of a tall body and left a connected
    // node reading as a picture with no visible wiring.
    static let thumbHeight: CGFloat = 52
    static let height: CGFloat = headerHeight + thumbHeight + 18
    static let portRadius: CGFloat = 5.5
    static let portHit: CGFloat = 13

    static func inputPoint(_ node: GraphNode, port: Int) -> CGPoint {
        if port == maskPort {
            return CGPoint(x: node.position.x, y: node.position.y + height - 11)
        }
        let n = max(1, node.kind.fieldInputs)
        let span = thumbHeight * 0.6
        let top = node.position.y + headerHeight + 16
        let step = n > 1 ? span / CGFloat(n - 1) : 0
        return CGPoint(x: node.position.x, y: top + CGFloat(port) * step)
    }

    static func outputPoint(_ node: GraphNode) -> CGPoint {
        CGPoint(x: node.position.x + width, y: node.position.y + headerHeight + 16)
    }

    static func rect(_ node: GraphNode) -> CGRect {
        CGRect(x: node.position.x, y: node.position.y, width: width, height: height)
    }

    /// Cubic bezier between two ports, bowed horizontally the way every node
    /// editor draws a link — the bow is what makes a dense graph readable.
    static func path(from: CGPoint, to: CGPoint) -> Path {
        var p = Path()
        let dx = max(40, abs(to.x - from.x) * 0.5)
        p.move(to: from)
        p.addCurve(to: to,
                   control1: CGPoint(x: from.x + dx, y: from.y),
                   control2: CGPoint(x: to.x - dx, y: to.y))
        return p
    }

    /// Distance from a point to a wire, by sampling the curve. Used for
    /// clicking a wire and for splicing a node into one.
    static func distance(to point: CGPoint, from a: CGPoint, to b: CGPoint) -> CGFloat {
        let dx = max(40, abs(b.x - a.x) * 0.5)
        let c1 = CGPoint(x: a.x + dx, y: a.y)
        let c2 = CGPoint(x: b.x - dx, y: b.y)
        var best = CGFloat.greatestFiniteMagnitude
        for i in 0...24 {
            let t = CGFloat(i) / 24
            let u = 1 - t
            let x = u*u*u*a.x + 3*u*u*t*c1.x + 3*u*t*t*c2.x + t*t*t*b.x
            let y = u*u*u*a.y + 3*u*u*t*c1.y + 3*u*t*t*c2.y + t*t*t*b.y
            best = min(best, hypot(x - point.x, y - point.y))
        }
        return best
    }
}

// ---------------------------------------------------------------------------
// The drawer
// ---------------------------------------------------------------------------

struct NodeGraphDrawer: View {
    @ObservedObject var model: EngineModel
    @State private var selection: UUID? = nil
    @State private var selectedEdge: UUID? = nil

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            HSplitView {
                // The canvas needs a real minimum height of its own. Without
                // one the drawer has no intrinsic size to defend, and the
                // enclosing VSplitView compresses it past the minHeight set on
                // the drawer itself — which is what clipped the control strip
                // off the bottom edge.
                // 330 is not arbitrary: the search palette is 288pt tall, and
                // a canvas shorter than that cannot show one without it
                // scrolling or being clamped against an edge. The node cards
                // are 96pt, so this also leaves room for two rows of graph
                // rather than one.
                GraphCanvas(model: model, selection: $selection, selectedEdge: $selectedEdge)
                    .frame(minWidth: 420, minHeight: 330)
                inspector
                    .frame(minWidth: 220, idealWidth: 268, maxWidth: 340)
            }
        }
        .background(Color(nsColor: .underPageBackgroundColor))
    }

    private var header: some View {
        HStack(spacing: 10) {
            Label("Node Graph", systemImage: "point.3.connected.trianglepath.dotted")
                .font(.system(size: 11, weight: .bold))

            if model.graphMode {
                Text("DRIVING TERRAIN")
                    .font(.system(size: 8, weight: .bold))
                    .kerning(0.8)
                    .padding(.horizontal, 6).padding(.vertical, 2)
                    .background(Color.accentColor.opacity(0.25), in: Capsule())
            }

            Spacer()

            if let stackable = model.graph.asStack() {
                Button {
                    model.applyGraphToStack()
                    model.graphDrawerOpen = false
                } label: {
                    Label("Bake to Stack", systemImage: "square.stack.3d.down.right")
                        .font(.system(size: 10))
                }
                .help("This graph is a straight line, so it can go back to the layer stack as \(stackable.count) layer\(stackable.count == 1 ? "" : "s").")
            } else {
                Text("Forked — cannot be a layer stack")
                    .font(.system(size: 9))
                    .foregroundStyle(.secondary)
            }

            Button {
                model.graphMode = false
                model.graphDrawerOpen = false
                model.rebuild()
            } label: {
                Label("Back to Stack", systemImage: "list.bullet.indent").font(.system(size: 10))
            }
            .help("Leave the graph as it is and let the layer stack drive the terrain again.")

            Button {
                model.graphDrawerOpen = false
            } label: {
                Image(systemName: "chevron.down").font(.system(size: 10, weight: .bold))
            }
            .buttonStyle(.plain)
            .help("Hide the drawer (⌥⌘G). The graph keeps driving the terrain.")
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
    }

    // MARK: Inspector

    private var inspector: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                if let id = selection, let index = model.graph.index(id) {
                    nodeInspector(index: index)
                } else {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("No node selected")
                            .font(.system(size: 11, weight: .semibold))
                        Text("Double-click the canvas to add a node. Drag from a port to wire "
                             + "two together, or drop a wire on empty space to make its "
                             + "destination. Drop an unconnected node onto a wire to splice it in.")
                            .font(.system(size: 10))
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    @ViewBuilder
    private func nodeInspector(index: Int) -> some View {
        let node = model.graph.nodes[index]
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 6) {
                Image(systemName: node.kind.symbol).font(.system(size: 11))
                TextField(node.kind.label, text: Binding(
                    get: { model.graph.nodes[index].name },
                    set: { model.graph.nodes[index].name = $0 }))
                    .textFieldStyle(.plain)
                    .font(.system(size: 12, weight: .bold))
            }
            Text(node.kind.blurb)
                .font(.system(size: 9.5))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            if node.kind != .output {
                Toggle("Enabled", isOn: Binding(
                    get: { model.graph.nodes[index].enabled },
                    set: { model.graph.nodes[index].enabled = $0; rerun() }))
                    .font(.system(size: 10))
                    .toggleStyle(.checkbox)
            }

            Divider()

            if node.kind.params.isEmpty {
                Text(node.kind == .output
                     ? "Whatever reaches this node is the terrain."
                     : "No parameters.")
                    .font(.system(size: 10))
                    .foregroundStyle(.secondary)
            }

            ForEach(node.kind.params, id: \.key) { def in
                paramControl(index: index, def: def)
            }

            if node.kind == .curve {
                Text("Height Remap").font(.system(size: 9, weight: .bold))
                    .foregroundStyle(.secondary).kerning(1.1)
                CurveEditorView(points: model.graph.nodes[index].curve ?? defaultCurve,
                                onChange: { pts in
                                    model.graph.nodes[index].curve = pts
                                    rerun()
                                })
                    .frame(height: 150)
            }
        }
    }

    @ViewBuilder
    private func paramControl(index: Int, def: ParamDef) -> some View {
        let binding = Binding<Double>(
            get: { model.graph.nodes[index].params[def.key] ?? def.def },
            set: { model.graph.nodes[index].params[def.key] = $0 })

        if let choices = def.choices {
            HStack {
                Text(def.label).font(.system(size: 10))
                Spacer()
                Picker("", selection: Binding(
                    get: { Int(binding.wrappedValue.rounded()) },
                    set: { binding.wrappedValue = Double($0); rerun() })) {
                    ForEach(choices.indices, id: \.self) { i in
                        Text(choices[i]).tag(i)
                    }
                }
                .labelsHidden()
                .frame(width: 108)
            }
        } else {
            VStack(spacing: 2) {
                HStack {
                    Text(def.label).font(.system(size: 10))
                    Spacer()
                    Text(String(format: def.step >= 1 ? "%.0f" : "%.2f", binding.wrappedValue))
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
                Slider(value: binding, in: def.range, step: def.step) { editing in
                    if !editing { rerun() }
                }
            }
        }
    }

    private func rerun() {
        model.graphMode = true
        model.rebuildFromGraph()
    }
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

struct GraphCanvas: View {
    @ObservedObject var model: EngineModel
    @Binding var selection: UUID?
    @Binding var selectedEdge: UUID?

    @State private var pan = CGSize(width: 20, height: 10)
    @State private var panStart = CGSize.zero
    @State private var zoom: CGFloat = 1
    @State private var zoomStart: CGFloat = 1
    @State private var dragOffset: CGSize = .zero
    @State private var draggingNode: UUID? = nil
    /// A wire being pulled from a port: its origin and the live cursor point.
    @State private var pending: (from: UUID, at: CGPoint, cursor: CGPoint)? = nil
    @State private var palette: (point: CGPoint, connectFrom: UUID?)? = nil
    @State private var paletteQuery = ""
    /// Framed once, when the canvas first has a real size to frame against.
    @State private var hasFramed = false
    /// onDeleteCommand only reaches a view that holds keyboard focus. Without
    /// this the canvas was never in the responder chain, so selecting a wire
    /// or a node and pressing Delete did nothing at all.
    @FocusState private var canvasFocused: Bool
    /// The palette's search field. It opens to be typed into — that is the
    /// whole point of a search palette — and it was never given focus, so the
    /// first thing you typed went nowhere and the list stayed unfiltered.
    @FocusState private var searchFocused: Bool

    /// The canvas's own coordinate space: unscaled and unpanned, so a gesture
    /// location converts to graph space through toGraph and nothing else.
    static let canvasSpace = "titanGraphCanvas"

    private var graph: TerrainGraph { model.graph }

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .topLeading) {
                background

                ZStack(alignment: .topLeading) {
                    wires
                    ForEach(graph.nodes) { node in
                        nodeCard(node)
                            .frame(width: NodeGeometry.width, height: NodeGeometry.height)
                            .offset(x: offsetFor(node).x, y: offsetFor(node).y)
                    }
                }
                .scaleEffect(zoom, anchor: .topLeading)
                .offset(pan)
                .allowsHitTesting(palette == nil)

                if let palette {
                    paletteView(at: palette.point, connectFrom: palette.connectFrom, in: geo.size)
                }

                overlayControls(geo: geo)
            }
            .focusable()
            .focused($canvasFocused)
            // Focusable for the Delete key's sake, not to be decorated for it.
            // The ring AppKit draws lands on the canvas's layout bounds rather
            // than its visible frame, so it appeared as a stray blue rule down
            // the edge of the sidebar.
            .focusEffectDisabled()
            // Every gesture that needs a position measures it here, in
            // unscaled, unpanned canvas coordinates, and converts with
            // toGraph. This name used to be referenced by the two port
            // gestures and declared by nobody: an unresolved coordinate space
            // does not raise, it just reports a location in some other space,
            // so toGraph turned it into a graph point far from the cursor and
            // inputHit almost never matched. Dragging a wire between two ports
            // simply did not connect.
            .coordinateSpace(name: Self.canvasSpace)
            .clipped()
            .onDeleteCommand { deleteSelection() }
            // Open framed on the graph. The starter layout puts its nodes at
            // y=240, and the drawer is nowhere near 240pt tall, so without this
            // the canvas opens on empty grid with every node below the fold —
            // and the Fit button that would rescue it is in the same clipped
            // strip. Framing on appear is what every node editor does anyway.
            .onAppear { frameIfNeeded(geo.size) }
            .onChange(of: geo.size) { _ in frameIfNeeded(geo.size) }
        }
    }

    /// Frames the graph the first time the canvas has a usable size. A
    /// GeometryReader reports .zero on the first pass, so this cannot simply
    /// run in onAppear and be done with it.
    private func frameIfNeeded(_ size: CGSize) {
        guard !hasFramed, size.width > 1, size.height > 1, !graph.nodes.isEmpty else { return }
        hasFramed = true
        fit(in: size)
    }

    // MARK: Background, pan and zoom

    private var background: some View {
        Rectangle()
            .fill(Color(nsColor: .textBackgroundColor).opacity(0.35))
            .overlay(GridPattern(spacing: 24 * zoom, offset: pan))
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 1)
                    .onChanged { v in
                        pan = CGSize(width: panStart.width + v.translation.width,
                                     height: panStart.height + v.translation.height)
                    }
                    .onEnded { _ in panStart = pan })
            .simultaneousGesture(
                MagnificationGesture()
                    .onChanged { v in zoom = min(2.0, max(0.35, zoomStart * v)) }
                    .onEnded { _ in zoomStart = zoom })
            .onTapGesture(count: 2) { location in
                openPalette(at: location, connectFrom: nil)
            }
            .onTapGesture { location in
                if let edge = wireHit(at: toGraph(location)) {
                    selectedEdge = edge; canvasFocused = true
                    selection = nil
                } else {
                    selection = nil
                    selectedEdge = nil
                }
            }
    }

    private func toGraph(_ p: CGPoint) -> CGPoint {
        CGPoint(x: (p.x - pan.width) / zoom, y: (p.y - pan.height) / zoom)
    }

    private func offsetFor(_ node: GraphNode) -> CGPoint {
        let dragging = draggingNode == node.id
        return CGPoint(x: node.position.x + (dragging ? dragOffset.width : 0),
                       y: node.position.y + (dragging ? dragOffset.height : 0))
    }

    /// The node as the wires should see it while it is being dragged.
    private func live(_ node: GraphNode) -> GraphNode {
        guard draggingNode == node.id else { return node }
        var copy = node
        copy.position = CGPoint(x: node.position.x + dragOffset.width,
                                y: node.position.y + dragOffset.height)
        return copy
    }

    // MARK: Wires

    private var wires: some View {
        Canvas { ctx, _ in
            for edge in graph.edges {
                guard let a = graph.node(edge.from), let b = graph.node(edge.to) else { continue }
                let start = NodeGeometry.outputPoint(live(a))
                let end = NodeGeometry.inputPoint(live(b), port: edge.port)
                let selected = selectedEdge == edge.id
                let color: Color = edge.port == maskPort ? .purple : .accentColor
                ctx.stroke(NodeGeometry.path(from: start, to: end),
                           with: .color(selected ? .white : color.opacity(0.85)),
                           style: StrokeStyle(lineWidth: selected ? 3 : 2, lineCap: .round,
                                              dash: edge.port == maskPort ? [5, 3] : []))
            }
            if let pending {
                ctx.stroke(NodeGeometry.path(from: pending.at, to: pending.cursor),
                           with: .color(.white.opacity(0.7)),
                           style: StrokeStyle(lineWidth: 2, dash: [4, 4]))
            }
        }
        .frame(width: 6000, height: 4000)
        .allowsHitTesting(false)
    }

    private func wireHit(at point: CGPoint) -> UUID? {
        var best: (UUID, CGFloat)? = nil
        for edge in graph.edges {
            guard let a = graph.node(edge.from), let b = graph.node(edge.to) else { continue }
            let d = NodeGeometry.distance(to: point,
                                          from: NodeGeometry.outputPoint(a),
                                          to: NodeGeometry.inputPoint(b, port: edge.port))
            if d < 10, best == nil || d < best!.1 { best = (edge.id, d) }
        }
        return best?.0
    }

    // MARK: Node card

    private func nodeCard(_ node: GraphNode) -> some View {
        let isSelected = selection == node.id
        let isPreviewed = graph.previewNode == node.id
            || (graph.previewNode == nil && node.kind == .output)

        return ZStack(alignment: .topLeading) {
            RoundedRectangle(cornerRadius: 7)
                .fill(Color(nsColor: .controlBackgroundColor).opacity(0.97))
                .overlay(RoundedRectangle(cornerRadius: 7)
                    .stroke(isSelected ? Color.accentColor
                            : (isPreviewed ? Color.orange.opacity(0.8) : Color.black.opacity(0.45)),
                            lineWidth: isSelected || isPreviewed ? 2 : 1))
                .shadow(color: .black.opacity(0.35), radius: 4, y: 2)
                .opacity(node.enabled ? 1 : 0.55)

            VStack(alignment: .leading, spacing: 0) {
                HStack(spacing: 5) {
                    Image(systemName: node.kind.symbol).font(.system(size: 9))
                    Text(node.title).font(.system(size: 10, weight: .semibold)).lineLimit(1)
                    Spacer(minLength: 2)
                    Button {
                        graph.previewNode = isPreviewed && node.kind != .output ? nil : node.id
                        rerun()
                    } label: {
                        Image(systemName: isPreviewed ? "eye.fill" : "eye")
                            .font(.system(size: 9))
                            .foregroundStyle(isPreviewed ? Color.orange : Color.secondary)
                    }
                    .buttonStyle(.plain)
                    .help("Show this node's result in the viewport")
                }
                .padding(.horizontal, 8)
                .frame(height: NodeGeometry.headerHeight)

                thumbnail(node)
                    .frame(height: NodeGeometry.thumbHeight - 6)
                    .padding(.horizontal, 8)
            }

            ports(node)
        }
        .contentShape(RoundedRectangle(cornerRadius: 7))
        .onTapGesture { selection = node.id; selectedEdge = nil; canvasFocused = true }
        // Measured in canvas space and converted, rather than taking
        // DragGesture's own translation and dividing by zoom. The card sits
        // inside a .scaleEffect, and what a local-space translation means
        // under one is exactly the ambiguity that sent a dragged node flying:
        // the divide double-compensated, so the node moved 1/zoom further than
        // the cursor and a zoomed-out canvas threw it clean off the screen.
        // The difference of two converted points has no such ambiguity — it is
        // the graph-space distance the cursor actually travelled.
        .gesture(
            DragGesture(minimumDistance: 2, coordinateSpace: .named(Self.canvasSpace))
                .onChanged { v in
                    if draggingNode != node.id { draggingNode = node.id; selection = node.id }
                    let from = toGraph(v.startLocation)
                    let to = toGraph(v.location)
                    dragOffset = CGSize(width: to.x - from.x, height: to.y - from.y)
                }
                .onEnded { v in
                    let from = toGraph(v.startLocation)
                    let to = toGraph(v.location)
                    if let i = graph.index(node.id) {
                        graph.nodes[i].position.x += to.x - from.x
                        graph.nodes[i].position.y += to.y - from.y
                        spliceIfDroppedOnWire(graph.nodes[i])
                    }
                    draggingNode = nil
                    dragOffset = .zero
                })
    }

    @ViewBuilder
    private func thumbnail(_ node: GraphNode) -> some View {
        if let image = model.nodeThumbnails[node.id] {
            // .fill on a square image in a wide, short slot scales it to the
            // width and lets the rest hang out of the card. clipShape hides
            // the overflow but the view is still there to be hit, so an
            // evaluated node covered its own ports and everything under it:
            // once a node had a thumbnail it could no longer be dragged or
            // rewired, only selected. .fit keeps it inside, clipped() bounds
            // it, and the image takes no hits at all — it is decoration.
            Image(decorative: image, scale: 1)
                .resizable()
                .interpolation(.medium)
                .aspectRatio(contentMode: .fit)
                .frame(maxWidth: .infinity)
                .clipped()
                .clipShape(RoundedRectangle(cornerRadius: 3))
                .allowsHitTesting(false)
        } else {
            RoundedRectangle(cornerRadius: 3)
                .fill(Color.black.opacity(0.25))
                .overlay(Text(node.kind == .output ? "viewport" : "—")
                    .font(.system(size: 9)).foregroundStyle(.secondary))
        }
    }

    private func ports(_ node: GraphNode) -> some View {
        ZStack(alignment: .topLeading) {
            ForEach(0..<node.kind.fieldInputs, id: \.self) { port in
                portDot(color: .accentColor, filled: graph.source(of: node.id, port: port) != nil)
                    .position(relative(NodeGeometry.inputPoint(node, port: port), to: node))
                    .gesture(inputDragGesture(node: node, port: port))
                    .help(node.kind.inputLabels.indices.contains(port)
                          ? node.kind.inputLabels[port] : "Input")
            }
            if node.kind.takesMask {
                portDot(color: .purple, filled: graph.source(of: node.id, port: maskPort) != nil)
                    .position(relative(NodeGeometry.inputPoint(node, port: maskPort), to: node))
                    .gesture(inputDragGesture(node: node, port: maskPort))
                    .help("Mask — limits this node to part of the terrain")
            }
            if node.kind.hasOutput {
                portDot(color: .accentColor, filled: !graph.consumers(of: node.id).isEmpty)
                    .position(relative(NodeGeometry.outputPoint(node), to: node))
                    .gesture(outputDragGesture(node: node))
            }
        }
    }

    private func relative(_ point: CGPoint, to node: GraphNode) -> CGPoint {
        CGPoint(x: point.x - node.position.x, y: point.y - node.position.y)
    }

    private func portDot(color: Color, filled: Bool) -> some View {
        Circle()
            .fill(filled ? color : Color(nsColor: .controlBackgroundColor))
            .overlay(Circle().stroke(color, lineWidth: 1.5))
            .frame(width: NodeGeometry.portRadius * 2, height: NodeGeometry.portRadius * 2)
            .contentShape(Circle().size(width: NodeGeometry.portHit, height: NodeGeometry.portHit)
                .offset(x: -(NodeGeometry.portHit - NodeGeometry.portRadius * 2) / 2,
                        y: -(NodeGeometry.portHit - NodeGeometry.portRadius * 2) / 2))
    }

    // MARK: Wiring gestures

    private func outputDragGesture(node: GraphNode) -> some Gesture {
        DragGesture(minimumDistance: 1, coordinateSpace: .named(Self.canvasSpace))
            .onChanged { v in
                pending = (node.id, NodeGeometry.outputPoint(node), toGraph(v.location))
            }
            .onEnded { v in
                let drop = toGraph(v.location)
                defer { pending = nil }
                if let target = inputHit(at: drop) {
                    graph.connect(from: node.id, to: target.node, port: target.port)
                    rerun()
                } else if graph.nodes.first(where: { NodeGeometry.rect($0).contains(drop) }) == nil {
                    // Dropping a wire on empty canvas offers to make its
                    // destination, pre-wired — the fastest way to extend a
                    // graph, and every editor that has it is better for it.
                    openPalette(at: v.location, connectFrom: node.id)
                }
            }
    }

    /// Dragging *from* an input pulls the existing wire off, which is how a
    /// connection is undone everywhere else.
    private func inputDragGesture(node: GraphNode, port: Int) -> some Gesture {
        DragGesture(minimumDistance: 1, coordinateSpace: .named(Self.canvasSpace))
            .onChanged { v in
                if let src = graph.source(of: node.id, port: port), let s = graph.node(src) {
                    pending = (src, NodeGeometry.outputPoint(s), toGraph(v.location))
                }
            }
            .onEnded { v in
                defer { pending = nil }
                guard let src = graph.source(of: node.id, port: port) else { return }
                graph.disconnect(to: node.id, port: port)
                if let target = inputHit(at: toGraph(v.location)) {
                    graph.connect(from: src, to: target.node, port: target.port)
                }
                rerun()
            }
    }

    private func inputHit(at point: CGPoint) -> (node: UUID, port: Int)? {
        for node in graph.nodes {
            for port in 0..<node.kind.fieldInputs {
                if hypot(point.x - NodeGeometry.inputPoint(node, port: port).x,
                         point.y - NodeGeometry.inputPoint(node, port: port).y) < 18 {
                    return (node.id, port)
                }
            }
            if node.kind.takesMask {
                let p = NodeGeometry.inputPoint(node, port: maskPort)
                if hypot(point.x - p.x, point.y - p.y) < 18 { return (node.id, maskPort) }
            }
        }
        // Dropped on a node's body rather than exactly on a port: take its
        // first free input, then its first input. Precision should not be the
        // price of connecting two nodes.
        if let node = graph.nodes.first(where: { NodeGeometry.rect($0).contains(point) }),
           node.kind.fieldInputs > 0 {
            let free = (0..<node.kind.fieldInputs).first { graph.source(of: node.id, port: $0) == nil }
            return (node.id, free ?? 0)
        }
        return nil
    }

    private func spliceIfDroppedOnWire(_ node: GraphNode) {
        guard graph.edges.allSatisfy({ $0.from != node.id && $0.to != node.id }) else { return }
        let center = CGPoint(x: node.position.x + NodeGeometry.width / 2,
                             y: node.position.y + NodeGeometry.height / 2)
        var best: (GraphEdge, CGFloat)? = nil
        for edge in graph.edges {
            guard let a = graph.node(edge.from), let b = graph.node(edge.to) else { continue }
            let d = NodeGeometry.distance(to: center,
                                          from: NodeGeometry.outputPoint(a),
                                          to: NodeGeometry.inputPoint(b, port: edge.port))
            if d < 44, best == nil || d < best!.1 { best = (edge, d) }
        }
        if let (edge, _) = best {
            graph.insert(node.id, into: edge)
            rerun()
        }
    }

    // MARK: Palette

    private func openPalette(at point: CGPoint, connectFrom: UUID?) {
        paletteQuery = ""
        palette = (point, connectFrom)
    }

    private func paletteView(at point: CGPoint, connectFrom: UUID?,
                             in canvas: CGSize) -> some View {
        let matches = NodeKind.allCases.filter { kind in
            guard kind != .output else { return false }
            guard connectFrom == nil || kind.fieldInputs > 0 else { return false }
            guard !paletteQuery.isEmpty else { return true }
            return kind.label.localizedCaseInsensitiveContains(paletteQuery)
                || kind.category.localizedCaseInsensitiveContains(paletteQuery)
        }

        return VStack(alignment: .leading, spacing: 0) {
            TextField("Search nodes…", text: $paletteQuery)
                .textFieldStyle(.plain)
                .font(.system(size: 12))
                .padding(8)
                .focused($searchFocused)
                .onAppear { searchFocused = true }
                .onSubmit { if let first = matches.first { place(first, at: point, from: connectFrom) } }
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 1) {
                    ForEach(matches) { kind in
                        Button {
                            place(kind, at: point, from: connectFrom)
                        } label: {
                            HStack(spacing: 6) {
                                Image(systemName: kind.symbol).font(.system(size: 10)).frame(width: 14)
                                Text(kind.label).font(.system(size: 11))
                                Spacer()
                                Text(kind.category).font(.system(size: 9)).foregroundStyle(.secondary)
                            }
                            .padding(.horizontal, 8).padding(.vertical, 4)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.vertical, 3)
            }
            .frame(maxHeight: 240)
        }
        .frame(width: Self.paletteWidth)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.black.opacity(0.3)))
        .shadow(radius: 12)
        // Kept inside the canvas. It used to be placed at the click point with
        // no regard for its own size, so opening it near the right or bottom
        // edge — which in a drawer this short is most of it — pushed the
        // category column under the inspector and the list off the end of the
        // window, where neither could be reached.
        .offset(x: min(max(0, point.x), max(0, canvas.width - Self.paletteWidth)),
                y: min(max(0, point.y), max(0, canvas.height - Self.paletteHeight)))
        .onExitCommand { palette = nil }
    }

    private func place(_ kind: NodeKind, at screenPoint: CGPoint, from: UUID?) {
        let p = toGraph(screenPoint)
        let id = graph.add(kind, at: CGPoint(x: p.x - 20, y: p.y - 20))
        if let from { graph.connect(from: from, to: id, port: 0) }
        selection = id
        palette = nil
        rerun()
    }

    // MARK: Overlay controls

    private func overlayControls(geo: GeometryProxy) -> some View {
        // Pinned to the top, not the bottom. The drawer is short and gets
        // squeezed further by the split view, and a bottom-anchored strip was
        // the first thing to fall off the edge — taking Fit, Add and the zoom
        // controls with it, so a graph that opened out of view could not be
        // brought back. The top edge is the one that is always on screen.
        VStack {
            HStack(spacing: 6) {
                Button { openPalette(at: CGPoint(x: 60, y: 60), connectFrom: nil) } label: {
                    Image(systemName: "plus")
                }
                .help("Add a node (or double-click the canvas)")
                Button { zoom = min(2.0, zoom * 1.2); zoomStart = zoom } label: {
                    Image(systemName: "plus.magnifyingglass")
                }
                Button { zoom = max(0.35, zoom / 1.2); zoomStart = zoom } label: {
                    Image(systemName: "minus.magnifyingglass")
                }
                Button { fit(in: geo.size) } label: { Image(systemName: "arrow.up.left.and.arrow.down.right") }
                    .help("Fit the graph in view")
                // Deletion needs a button, not only the Delete key. The key
                // routes through onDeleteCommand, which reaches whichever view
                // holds focus — fine once the canvas is focusable, but it
                // silently does nothing the moment focus is anywhere else, and
                // there was no other way to remove a wire at all.
                Button { deleteSelection() } label: { Image(systemName: "trash") }
                    .disabled(selection == nil && selectedEdge == nil)
                    .help(selectedEdge != nil ? "Delete the selected wire (⌫)"
                                              : "Delete the selected node (⌫)")
                Text("\(Int(zoom * 100))%")
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(.secondary)
                Spacer()
                if model.isGenerating {
                    ProgressView().controlSize(.small)
                }
                Text(model.statusText)
                    .font(.system(size: 9))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(.ultraThinMaterial)
            Spacer()
        }
    }

    /// Height the control strip occupies, kept clear so a framed graph is not
    /// tucked underneath it.
    private static let controlStrip: CGFloat = 30

    /// The palette's own size, so it can be kept inside the canvas.
    private static let paletteWidth: CGFloat = 240
    private static let paletteHeight: CGFloat = 288

    private func fit(in size: CGSize) {
        guard !graph.nodes.isEmpty else { return }
        let minX = graph.nodes.map(\.position.x).min()! - 24
        let minY = graph.nodes.map(\.position.y).min()! - 24
        let maxX = graph.nodes.map(\.position.x).max()! + NodeGeometry.width + 24
        let maxY = graph.nodes.map(\.position.y).max()! + NodeGeometry.height + 24
        let usable = max(1, size.height - Self.controlStrip)
        let scale = min(size.width / max(1, maxX - minX), usable / max(1, maxY - minY))
        zoom = min(1.4, max(0.35, scale))
        zoomStart = zoom
        // Centre what is left over, so a small graph does not sit jammed into
        // the top-left corner of a wide canvas.
        let slackX = max(0, size.width - (maxX - minX) * zoom) / 2
        let slackY = max(0, usable - (maxY - minY) * zoom) / 2
        pan = CGSize(width: -minX * zoom + slackX,
                     height: -minY * zoom + slackY + Self.controlStrip)
        panStart = pan
    }

    private func deleteSelection() {
        if let edge = selectedEdge {
            graph.edges.removeAll { $0.id == edge }
            selectedEdge = nil
            rerun()
        } else if let id = selection {
            graph.remove(id)
            selection = nil
            rerun()
        }
    }

    private func rerun() {
        model.graphMode = true
        model.rebuildFromGraph()
    }
}

/// The dotted backdrop every node editor has, for a sense of place while
/// panning.
struct GridPattern: View {
    let spacing: CGFloat
    let offset: CGSize

    var body: some View {
        Canvas { ctx, size in
            guard spacing > 6 else { return }
            let ox = offset.width.truncatingRemainder(dividingBy: spacing)
            let oy = offset.height.truncatingRemainder(dividingBy: spacing)
            var x = ox - spacing
            while x < size.width {
                var y = oy - spacing
                while y < size.height {
                    ctx.fill(Path(ellipseIn: CGRect(x: x, y: y, width: 1.6, height: 1.6)),
                             with: .color(.gray.opacity(0.35)))
                    y += spacing
                }
                x += spacing
            }
        }
        .allowsHitTesting(false)
    }
}
