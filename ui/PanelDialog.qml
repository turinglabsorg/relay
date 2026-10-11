import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    default property alias body: column.data
    property alias actions: footerRow.data
    property bool dark: true
    property real preferredWidth: 480
    property real windowWidth: 1180
    property real windowHeight: 780
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(preferredWidth, windowWidth - 32)
    height: Math.min(column.implicitHeight + topPadding + bottomPadding + (footer.visible ? footer.implicitHeight + spacing : 0), windowHeight - 32)
    padding: windowWidth < 700 ? 24 : 30
    bottomPadding: footerRow.children.length ? 0 : padding
    spacing: 24
    modal: true
    onAboutToShow: scroll.contentY = 0
    closePolicy: Popup.CloseOnEscape
    background: Rectangle {
        color: dialog.dark ? "#17221b" : "#ffffff"
        radius: 18; border.color: dialog.dark ? "#2b3c30" : "#d7e4d9"
    }
    contentItem: Flickable {
        id: scroll
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        ColumnLayout { id: column; width: scroll.width; spacing: 16 }
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded; active: true }
    }
    footer: Item {
        visible: footerRow.children.length > 0
        implicitHeight: visible ? footerRow.implicitHeight + dialog.padding : 0
        RowLayout {
            id: footerRow
            x: dialog.padding
            width: parent.width - 2 * dialog.padding
            spacing: 12
        }
    }
    Overlay.modal: Rectangle { color: "#99060e09" }
}
