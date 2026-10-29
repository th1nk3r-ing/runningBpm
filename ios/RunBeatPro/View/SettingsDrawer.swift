import SwiftUI

/// 左侧设置抽屉
struct SettingsDrawer: View {
    @Binding var isPresented: Bool
    @Environment(AppState.self) private var state

    @State private var dragX: CGFloat = 0
    @State private var showThemeSheet = false
    @State private var showTimbreSheet = false
    @State private var showAccentSheet = false

    var body: some View {
        GeometryReader { proxy in
            let width = min(proxy.size.width * 0.84, 340)

            ZStack(alignment: .leading) {
                Color.black
                    .opacity(isPresented ? 0.55 : 0)
                    .ignoresSafeArea()
                    .allowsHitTesting(isPresented)
                    .accessibilityHidden(true)
                    .onTapGesture { close() }

                panel
                    .frame(width: width)
                    .frame(maxHeight: .infinity, alignment: .top)
                    .background(Color(white: 0.10).ignoresSafeArea())
                    .offset(x: isPresented ? dragX : -width)
                    .simultaneousGesture(closeDrag(width: width))
                    .accessibilityAddTraits(.isModal)
                    .accessibilityAction(.escape) { close() }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
            .animation(.easeOut(duration: 0.25), value: isPresented)
        }
        .allowsHitTesting(isPresented)
        .accessibilityHidden(!isPresented)
        .onChange(of: isPresented) { _, opened in
            if opened { dragX = 0 }
        }
    }

    // MARK: - 面板

    private var panel: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                Text(localized("settings_title"))
                    .font(.system(size: 20, weight: .bold))
                    .foregroundColor(.textPrimary)

                Spacer()

                Button {
                    close()
                } label: {
                    Image(systemName: "xmark")
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundColor(state.accentColor)
                        .frame(width: 44, height: 44)
                }
            }
            .padding(.leading, 16)
            .padding(.trailing, 4)
            .padding(.top, 8)

            ScrollView {
                VStack(alignment: .leading, spacing: 8) {
                    sectionTitle("settings_section_general")
                    languageRow
                    themeRow

                    sectionTitle("settings_section_sound")
                    valueRow(label: "label_timbre", value: state.timbreName) {
                        showTimbreSheet = true
                    }
                    valueRow(label: "label_accent", value: state.accentModeName) {
                        showAccentSheet = true
                    }

                    sectionTitle("settings_section_advanced")
                    logRow

                    Text(versionText)
                        .font(.system(size: 11))
                        .foregroundColor(.textSecondary)
                        .frame(maxWidth: .infinity, alignment: .center)
                        .padding(.top, 24)
                }
                .padding(.horizontal, 16)
                .padding(.top, 4)
                .padding(.bottom, 24)
            }
            .scrollIndicators(.hidden)
        }
        .sheet(isPresented: $showThemeSheet) { ThemePickerSheet() }
        .sheet(isPresented: $showTimbreSheet) { TimbrePickerSheet() }
        .sheet(isPresented: $showAccentSheet) { AccentPickerSheet() }
    }

    // MARK: - 行

    private var languageRow: some View {
        Button {
            state.currentLanguage = state.currentLanguage == "zh" ? "en" : "zh"
        } label: {
            rowContainer {
                rowLabel("label_language")
                Spacer()
                rowValue(state.currentLanguage == "zh" ? "中文" : "English")
            }
        }
        .buttonStyle(.plain)
    }

    private var themeRow: some View {
        Button {
            showThemeSheet = true
        } label: {
            rowContainer {
                rowLabel("dialog_theme_title")
                Spacer()
                Circle()
                    .fill(state.accentColor)
                    .frame(width: 20, height: 20)
                rowValue(state.accentColorName)
                    .padding(.leading, 6)
                chevron
            }
        }
        .buttonStyle(.plain)
    }

    private func valueRow(label: String, value: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            rowContainer {
                rowLabel(label)
                Spacer()
                rowValue(value)
                chevron
            }
        }
        .buttonStyle(.plain)
    }

    private var logRow: some View {
        Toggle(isOn: Binding(
            get: { state.logToFile },
            set: { state.logToFile = $0 }
        )) {
            HStack(spacing: 0) {
                rowLabel("label_log_to_file")
                Spacer()
                Text(state.logToFile ? localized("log_on") : localized("log_off"))
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(state.logToFile ? state.accentColor : .textSecondary)
            }
        }
        .toggleStyle(.switch)
        .tint(state.accentColor)
        .accessibilityLabel(localized("label_log_to_file"))
        .padding(.horizontal, 12)
        .frame(maxWidth: .infinity)
        .frame(height: 52)
        .background(
            RoundedRectangle(cornerRadius: 10)
                .stroke(state.accentColor.opacity(0.3), lineWidth: 1)
                .background(Color(white: 0.12).cornerRadius(10))
        )
    }

    private func rowContainer<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        HStack(spacing: 0) { content() }
            .padding(.horizontal, 12)
            .frame(maxWidth: .infinity)
            .frame(height: 52)
            .background(
                RoundedRectangle(cornerRadius: 10)
                    .stroke(state.accentColor.opacity(0.3), lineWidth: 1)
                    .background(Color(white: 0.12).cornerRadius(10))
            )
    }

    private func sectionTitle(_ key: String) -> some View {
        Text(localized(key))
            .font(.system(size: 12))
            .foregroundColor(.textSecondary)
            .tracking(1)
            .padding(.top, 8)
    }

    private func rowLabel(_ key: String) -> some View {
        Text(localized(key))
            .font(.system(size: 14))
            .foregroundColor(.textSecondary)
    }

    private func rowValue(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 14, weight: .bold))
            .foregroundColor(state.accentColor)
    }

    private var chevron: some View {
        Text("  ▾")
            .font(.system(size: 14))
            .foregroundColor(state.accentColor)
    }

    // MARK: - 关闭手势

    private func closeDrag(width: CGFloat) -> some Gesture {
        DragGesture(minimumDistance: 15)
            .onChanged { value in
                guard abs(value.translation.width) > abs(value.translation.height) else { return }
                dragX = max(-width, min(0, value.translation.width))
            }
            .onEnded { value in
                let horizontal = abs(value.translation.width) > abs(value.translation.height)
                if horizontal && (value.translation.width < -80 || value.predictedEndTranslation.width < -160) {
                    close()
                } else {
                    withAnimation(.easeOut(duration: 0.2)) { dragX = 0 }
                }
            }
    }

    private func close() {
        withAnimation(.easeOut(duration: 0.25)) {
            isPresented = false
        }
        dragX = 0
    }

    private var versionText: String {
        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.0"
        let build = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "1"
        return "v\(version) (\(build))"
    }

    private func localized(_ key: String) -> String {
        let table = "Localizable"
        let path = Bundle.main.path(forResource: state.currentLanguage, ofType: "lproj")
        let bundle = path.flatMap { Bundle(path: $0) } ?? .main
        return bundle.localizedString(forKey: key, value: nil, table: table)
    }
}
