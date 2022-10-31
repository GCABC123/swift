import SwiftParser
import SwiftSyntax

import CASTBridging

extension ASTGenVisitor {
  public func visit(_ node: CodeBlockSyntax) -> ASTNode {
    let statements = node.statements.map(self.visit).map { $0.bridged() }
    let loc = self.base.advanced(by: node.position.utf8Offset).raw

    return .stmt(statements.withBridgedArrayRef { ref in
      BraceStmt_create(ctx, loc, ref, loc)
    })
  }

  public func visit(_ node: IfStmtSyntax) -> ASTNode {
    let conditions = node.conditions.map(self.visit).map { $0.rawValue }
    assert(conditions.count == 1) // TODO: handle multiple conditions.
    
    let body = visit(node.body).rawValue
    let loc = self.base.advanced(by: node.position.utf8Offset).raw
    
    if let elseBody = node.elseBody, node.elseKeyword != nil {
      return .stmt(IfStmt_create(ctx, loc, conditions.first!, body, loc, visit(elseBody).rawValue))
    }
    
    return .stmt(IfStmt_create(ctx, loc, conditions.first!, body, nil, nil))
  }

  public func visit(_ node: ReturnStmtSyntax) -> ASTNode {
    let loc = self.base.advanced(by: node.position.utf8Offset).raw

    let expr: ASTNode?
    if let expression = node.expression {
      expr = visit(expression)
    } else {
      expr = nil
    }

    return .stmt(ReturnStmt_create(ctx, loc, expr?.rawValue))
  }
}
