import React, { Component } from 'react';
import { Redirect, Switch } from 'react-router';

import { AuthenticatedRoute } from '../authentication';

import EMSESPDashboard from './EMSESPDashboard';
import EMSESPSettings from './EMSESPSettings';

class ProjectRouting extends Component {

  render() {
    return (
      <Switch>
        <AuthenticatedRoute exact path="/ems-esp/status/*" component={EMSESPDashboard} />
        <AuthenticatedRoute exact path="/ems-esp/settings" component={EMSESPSettings} />
        <AuthenticatedRoute exact path="/ems-esp/*" component={EMSESPDashboard} />
        {
          /*
          * The redirect below caters for the default project route and redirecting invalid paths.
          * The "to" property must match one of the routes above for this to work correctly.
          */
        }
        <Redirect to={`/ems-esp/status`} />
      </Switch>
    )
  }

}

export default ProjectRouting;
