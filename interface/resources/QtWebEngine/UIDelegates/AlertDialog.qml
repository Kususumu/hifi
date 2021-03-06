import QtQuick 2.5

import "../../qml/dialogs"

QtObject {
    id: root
    signal accepted;
    property var text;

    property var messageDialogBuilder: Component { MessageDialog { } }

    function open() {
        console.log("prompt text " + text)
        var dialog = messageDialogBuilder.createObject(desktop, {
            text: root.text
        });

        dialog.selected.connect(function(button){
            accepted();
            dialog.destroy();
        });
    }
}
