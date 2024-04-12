import 'package:flutter/material.dart';

import '/components/items/permissions_list/permissions_list_widget.dart';
import '/flutter_flow/flutter_flow_util.dart';
import 'permission_page_widget.dart' show PermissionPageWidget;

class PermissionPageModel extends FlutterFlowModel<PermissionPageWidget> {
  ///  State fields for stateful widgets in this page.

  final unfocusNode = FocusNode();
  // State field(s) for PageView widget.
  PageController? pageViewController;

  int get pageViewCurrentIndex => pageViewController != null &&
          pageViewController!.hasClients &&
          pageViewController!.page != null
      ? pageViewController!.page!.round()
      : 0;
  // Model for permissionsList component.
  late PermissionsListModel permissionsListModel;

  @override
  void initState(BuildContext context) {
    permissionsListModel = createModel(context, () => PermissionsListModel());
  }

  @override
  void dispose() {
    unfocusNode.dispose();
    permissionsListModel.dispose();
  }
}
